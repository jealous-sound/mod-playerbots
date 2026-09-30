/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaHooks.h"
#include "CoaEventBus.h"
#include "CoaPlayerPrefs.h"
#include "CoaThread.h"
#include "CoaVoiceConfig.h"
#include "CoaVoiceDirector.h"
#include "Config.h"
#include "ScriptMgr.h"
#include <memory>
#include <vector>

namespace
{
// Every hook runs on the thread of World::Update. OnUpdate comes after MapMgr::Update has joined the map threads, so
// the director may touch any Player there; nothing posts to the bus while it drains.
class CoaWorldScript : public WorldScript
{
public:
    CoaWorldScript()
        : WorldScript("CoaWorldScript", {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE,
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
    }

    // Synchronous load: behind the single async worker it could wait for minutes at a first start.
    void OnStartup() override
    {
        CoaThread::CaptureWorldThread();
        if (CoaVoiceConfig::Enabled())
            CoaPlayerPrefs::Instance().LoadSync();
    }

    void OnUpdate(uint32 diff) override
    {
        CoaThread::CaptureWorldThread();
        // A no-op unless an async load is in flight (.reload config turned voice on, or .playerbots optout|optin).
        CoaPlayerPrefs::Instance().ProcessCallbacks();

        // With voice off the director is left alone (no lock, no snapshot copy), except for the one tick after it
        // was turned off, or with events still queued (on and off again within one tick): that tick drops what the
        // bus held and resets the director's state.
        bool const enabled = CoaVoiceConfig::Enabled();
        CoaEventBus& bus = CoaEventBus::Instance();
        if (!enabled && !directorRunning && !bus.Pending())
            return;

        bus.Drain(events);
        CoaVoiceDirector::Instance().Update(diff, events);
        events.clear();
        directorRunning = enabled;
    }

    // After the world loop: map threads are stopped, bots still exist. Late speech (Logout, KickAll) is dropped,
    // waiting lines are never sent, a load in flight is never processed. Queued writes are flushed by
    // PlayerbotsDatabase.Close() in OnDatabasesClosing.
    void OnShutdown() override
    {
        CoaThread::CaptureWorldThread();
        CoaEventBus::Instance().Close();
        CoaVoiceDirector::Instance().Shutdown();
        CoaPlayerPrefs::Instance().Shutdown();
    }

private:
    std::vector<CoaEvent> events;  // reused every tick
    bool directorRunning = false;  // the director was updated with voice on, and has not seen it off yet
};
}  // namespace

void AddSC_coa_world() { new CoaWorldScript(); }
