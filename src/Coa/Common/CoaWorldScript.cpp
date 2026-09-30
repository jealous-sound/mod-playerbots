/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaHooks.h"
#include "BrainAdmin.h"
#include "BrainConfig.h"
#include "BrainService.h"
#include "BrainUsageStore.h"
#include "Chat.h"
#include "CoaEventBus.h"
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
// the director may touch any Player there; nothing posts to the bus while it drains.
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

        CoaEventBus& bus = CoaEventBus::Instance();
        bus.Configure(sConfigMgr->GetOption<uint32>("AiPlayerbot.CoaHooks.BusMax", 20000));
        bus.SetEnabled(config->enable);

        // At startup OnStartup loads the preferences; here only a .reload config that turns voice on does (async).
        if (reload && config->enable)
            CoaPlayerPrefs::Instance().LoadAsync();

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
        if (CoaVoiceConfig::Enabled())
            CoaPlayerPrefs::Instance().LoadSync();

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

        // With voice off the director is left alone (no lock, no snapshot copy), except for the one tick after it
        // was turned off, or with events still queued (on and off again within one tick): that tick drops what the
        // bus held and resets the director's state. The director drains its brain results inside Update.
        bool const enabled = CoaVoiceConfig::Enabled();
        CoaEventBus& bus = CoaEventBus::Instance();
        if (!enabled && !directorRunning && !bus.Pending())
            return;

        bus.Drain(events);
        CoaVoiceDirector::Instance().Update(diff, events);
        events.clear();
        directorRunning = enabled;
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
    // included, are flushed by PlayerbotsDatabase.Close() in OnDatabasesClosing. The brain's workers get
    // CoaBrain.Shutdown.WorkerDeadlineMs, then are detached.
    void OnShutdown() override
    {
        CoaThread::CaptureWorldThread();
        CoaEventBus::Instance().Close();
        CoaVoiceDirector::Instance().Shutdown();
        bool const brainRan = BrainService::EverStarted();
        if (brainRan)
            BrainAdmin::Shutdown();
        BrainService::Shutdown();
        if (brainRan)
            BrainUsageStore::Instance().Flush();
        CoaPlayerPrefs::Instance().Shutdown();
    }

private:
    std::vector<CoaEvent> events;  // reused every tick
    bool directorRunning = false;  // the director was updated with voice on, and has not seen it off yet
    BrainAdminPrint const brainPrint = PrintBrainLine;  // built once: no std::function per tick
};
}  // namespace

void AddSC_coa_world() { new CoaWorldScript(); }
