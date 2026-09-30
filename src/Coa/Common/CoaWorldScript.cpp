/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaHooks.h"
#include "ActivityConfig.h"
#include "ActivityDirector.h"
#include "BrainAdmin.h"
#include "BrainConfig.h"
#include "BrainService.h"
#include "BrainUsageStore.h"
#include "Chat.h"
#include "CoaEventBus.h"
#include "CoaMemoryConfig.h"
#include "CoaMemoryMgr.h"
#include "CoaPlayerPrefs.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "CoaVoiceConfig.h"
#include "CoaVoiceDirector.h"
#include "Config.h"
#include "Log.h"
#include "QuestionRegistry.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <memory>
#include <string>
#include <vector>

namespace
{
// The answers of .playerbots brain test and probe arrive ticks after the command: the requester's session is looked
// up again by account, and a GM who logged out meanwhile gets nothing. Account 0 is the console.
void PrintBrainLine(uint32 account, std::string const& line)
{
    if (!account)
    {
        LOG_INFO("playerbots.brain", "{}", line);
        return;
    }

    WorldSession* const session = sWorldSessionMgr->FindSession(account);
    if (!session)
        return;

    // The reports use '|' as a separator, which the client reads as an escape code unless doubled.
    std::string text;
    text.reserve(line.size() + 8);
    for (char const c : line)
    {
        text += c;
        if (c == '|')
            text += '|';
    }
    ChatHandler(session).SendSysMessage(text);
}

// Every hook runs on the thread of World::Update. OnUpdate comes after MapMgr::Update has joined the map threads, so
// the directors and the memory manager may touch any Player there (and swap the bots' memory views); nothing posts to
// the bus while it drains.
class CoaWorldScript : public WorldScript
{
public:
    CoaWorldScript()
        : WorldScript("CoaWorldScript", {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE,
                                         WORLDHOOK_ON_SHUTDOWN_INITIATE, WORLDHOOK_ON_SHUTDOWN_CANCEL,
                                         WORLDHOOK_ON_SHUTDOWN})
    {
    }

    // Startup (World::LoadConfigSettings, before PlayerbotAIConfig::Initialize: nothing is read from it here) and
    // every .reload config.
    void OnAfterConfigLoad(bool reload) override
    {
        CoaThread::CaptureWorldThread();
        std::shared_ptr<CoaVoiceConfig const> const config = CoaVoiceConfig::Load();
        CoaVoiceConfig::Publish(config);

        // With CoaMemory.Enable = 0 this reads that one key.
        std::shared_ptr<CoaMemoryConfig const> const memory = CoaMemoryConfig::Load();
        CoaMemoryConfig::Publish(memory);

        // With CoaActivity.Enable = 0 this reads that one key.
        std::shared_ptr<ActivityConfig const> const activity = ActivityConfig::Load();
        ActivityConfig::Publish(activity);

        // The bus carries the voice's chat and the memory's deeds.
        CoaEventBus& bus = CoaEventBus::Instance();
        bus.Configure(sConfigMgr->GetOption<uint32>("AiPlayerbot.CoaHooks.BusMax", 20000));
        bus.SetEnabled(config->enable || memory->enable);

        // At startup OnStartup loads the preferences; here only a .reload config with voice, memory or activities on
        // does (async).
        if (reload && (config->enable || memory->enable || activity->enable))
            CoaPlayerPrefs::Instance().LoadAsync();
        // Off -> on: the online bots log in to memory over the next ticks. On -> off: memory stops at once with a
        // final flush; events still queued reach Update once more, which drops them.
        if (reload)
            CoaMemoryMgr::Instance().OnConfigReload();
        // Off -> on: the activity director starts (tables checked). On -> off: every activity ends at once.
        if (reload)
            ActivityDirector::Instance().OnConfigReload();

        // With CoaBrain.Enable = 0 this reads that one key. At startup the brain waits for OnStartup, where the
        // database is open: its usage must be loaded before it first runs. A reload ends .playerbots brain on|off.
        std::shared_ptr<BrainConfig const> const brain = BrainConfig::Load();
        BrainConfig::Publish(brain);
        if (reload)
        {
            BrainService::SetOverride(-1);
            if (brain->enable)
            {
                QuestionRegistry::Reload(brain->questionFile);
                BrainUsageStore::Instance().LoadSync();
            }
            BrainService::Apply(brain);
        }
    }

    // Synchronous loads: behind the single async worker they could wait for minutes at a first start.
    void OnStartup() override
    {
        CoaThread::CaptureWorldThread();
        if (CoaVoiceConfig::Enabled() || CoaMemoryConfig::Enabled() || ActivityConfig::Enabled())
            CoaPlayerPrefs::Instance().LoadSync();
        // After the preferences: the memory's first tick purges the accounts that opted out while it was off.
        CoaMemoryMgr::Instance().OnStartup();
        // With activities on: the tables are checked and the rows a crash left live are closed.
        ActivityDirector::Instance().OnStartup();

        std::shared_ptr<BrainConfig const> const brain = BrainConfig::Current();
        if (brain->enable)
        {
            QuestionRegistry::Reload(brain->questionFile);
            BrainUsageStore::Instance().LoadSync();
            BrainService::Apply(brain);
        }
    }

    void OnUpdate(uint32 diff) override
    {
        CoaThread::CaptureWorldThread();
        // A no-op unless an async load is in flight (.reload config turned voice on, or .playerbots optout|optin).
        CoaPlayerPrefs::Instance().ProcessCallbacks();

        // Before the voice block, which returns early with voice off: .playerbots brain test and probe work then too.
        // Still running after the brain is turned off, so the day's usage keeps being written.
        if (BrainService::EverStarted())
        {
            uint64 const now = CoaTime::NowMs();
            BrainUsageStore::Instance().Update(now);
            BrainAdmin::Update(now, brainPrint);
        }

        UpdateVoiceAndMemory(diff);

        // The activity director last (bus -> memory -> voice -> activity). Off: three relaxed loads; Restoring() keeps it
        // ticking while War Mode rulesets granted before activities were turned off still wait to be taken back.
        if (ActivityConfig::Enabled() || ActivityDirector::Running() || ActivityDirector::Restoring())
            ActivityDirector::Instance().Update(diff);
    }

    // .server shutdown|restart: no new brain job from here on; calls in flight finish. Signals, a closed console and
    // a Windows service stop go through World::StopNow, which skips this: OnShutdown covers them.
    void OnShutdownInitiate(ShutdownExitCode /*code*/, ShutdownMask /*mask*/) override
    {
        CoaThread::CaptureWorldThread();
        BrainService::ShutdownInitiate();
    }

    void OnShutdownCancel() override
    {
        CoaThread::CaptureWorldThread();
        BrainService::ShutdownCancel();
    }

    // After the world loop: map threads are stopped, bots still exist. Late speech (Logout, KickAll) is dropped,
    // waiting lines are never sent, a load in flight is never processed. Queued writes, the brain's last usage rows
    // and the memory's final flush included, are flushed by PlayerbotsDatabase.Close() in OnDatabasesClosing. The
    // brain's workers get CoaBrain.Shutdown.WorkerDeadlineMs, then are detached.
    void OnShutdown() override
    {
        CoaThread::CaptureWorldThread();
        CoaEventBus& bus = CoaEventBus::Instance();
        CoaMemoryMgr& memory = CoaMemoryMgr::Instance();
        // The deeds of the last tick are kept for the final flush; the voice events are dropped uncounted, as Close
        // drops them. With memory off Close alone frees the queue, as in M2.
        if (memory.Running())
        {
            bus.Drain(events);
            CoaMemoryMgr::TakeMemoryEvents(events, memoryEvents);
            events.clear();
        }
        bus.Close();
        // The director may hand the notes of its waiting decisions to the memory: it shuts down first.
        CoaVoiceDirector::Instance().Shutdown();
        memory.Shutdown(memoryEvents);
        memoryEvents.clear();
        // Every activity ends here (groups disbanded, rulesets restored, rows ended). Before the brain: the activity
        // director cancels no brain job, it only closes its sinks.
        ActivityDirector::Instance().Shutdown();
        bool const brainRan = BrainService::EverStarted();
        if (brainRan)
            BrainAdmin::Shutdown();
        BrainService::Shutdown();
        if (brainRan)
            BrainUsageStore::Instance().Flush();
        CoaPlayerPrefs::Instance().Shutdown();
    }

private:
    // The voice and memory part of OnUpdate, as it was in M3: its early returns leave only this function, so the
    // activity director after it runs whatever voice and memory do.
    void UpdateVoiceAndMemory(uint32 diff)
    {
        // With voice off the director is left alone (no lock, no snapshot copy), except for the one tick after it
        // was turned off, or with voice events still queued (on and off again within one tick): that tick drops
        // what the bus held and resets the director's state. The director drains its brain results inside Update.
        // Memory goes first (bus -> memory -> voice): the director's conversation notes of this tick then meet
        // memories already up to date. With memory off Running() is false and no memory event is queued, so these
        // statements reduce to M2's.
        bool const enabled = CoaVoiceConfig::Enabled();
        CoaEventBus& bus = CoaEventBus::Instance();
        CoaMemoryMgr& memory = CoaMemoryMgr::Instance();
        if (!enabled && !directorRunning && !memory.Running() && !bus.Pending())
            return;

        bus.Drain(events);
        CoaMemoryMgr::TakeMemoryEvents(events, memoryEvents);
        // Events left over from a memory turned off meanwhile reach Update once more, which drops them.
        if (memory.Running() || !memoryEvents.empty())
            memory.Update(diff, memoryEvents, brainPrint);
        memoryEvents.clear();
        if (!enabled && !directorRunning && events.empty())
            return;

        CoaVoiceDirector::Instance().Update(diff, events);
        events.clear();
        directorRunning = enabled;
    }

    std::vector<CoaEvent> events;        // reused every tick
    std::vector<CoaEvent> memoryEvents;  // the memory's share of `events`, reused every tick
    bool directorRunning = false;        // the director was updated with voice on, and has not seen it off yet
    BrainAdminPrint const brainPrint = PrintBrainLine;  // built once: no std::function per tick
};
}  // namespace

void AddSC_coa_world() { new CoaWorldScript(); }
