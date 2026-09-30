/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainService.h"
#include "BrainCache.h"
#include "BrainGovernor.h"
#include "BrainJson.h"
#include "CoaThread.h"
#include "InProcessBrain.h"
#include "Log.h"
#include "NullBrain.h"
#include <atomic>
#include <mutex>
#include <utility>

namespace
{
struct ServiceState
{
    std::mutex lock;  // guards `current` only: every other member is the world thread's
    std::shared_ptr<IBrain> const null = std::make_shared<NullBrain>();  // before `current`, which starts as it
    std::shared_ptr<IBrain> current;
    std::shared_ptr<InProcessBrain> running;
    std::atomic<bool> chatActive{false};
    std::atomic<bool> everStarted{false};
    int32 overrideState = -1;  // -1 follow the conf, 0 off, 1 on (.playerbots brain on|off)
    bool shutDown = false;
    bool jsonMissingLogged = false;
    std::string lastProblem;

    ServiceState() : current(null) {}
};

ServiceState& State()
{
    static ServiceState state;
    return state;
}

void Install(ServiceState& state, std::shared_ptr<IBrain> brain)
{
    std::lock_guard<std::mutex> guard(state.lock);
    state.current = brain ? std::move(brain) : state.null;
}

void StopRunning(ServiceState& state, uint32 deadlineMs)
{
    state.chatActive.store(false, std::memory_order_relaxed);
    Install(state, nullptr);
    if (state.running)
    {
        state.running->Shutdown(deadlineMs);
        state.running.reset();
    }
}

void UpdateChatActive(ServiceState& state)
{
    bool active = false;
    if (state.running && state.running->Accepting())
    {
        std::shared_ptr<BrainConfig const> const config = state.running->Config();
        active = config && config->useChatReply && config->AnyUsable();
    }
    state.chatActive.store(active, std::memory_order_relaxed);
}
}  // namespace

namespace BrainService
{
std::shared_ptr<IBrain> Current()
{
    ServiceState& state = State();
    std::lock_guard<std::mutex> guard(state.lock);
    return state.current;
}

bool ChatActive() { return State().chatActive.load(std::memory_order_relaxed); }

bool EverStarted() { return State().everStarted.load(std::memory_order_relaxed); }

void Apply(std::shared_ptr<BrainConfig const> config)
{
    COA_ASSERT_WORLD_THREAD();
    ServiceState& state = State();
    if (state.shutDown || !config)
        return;

    state.lastProblem.clear();
    BrainConfig::Publish(config);
    bool const wanted = state.overrideState >= 0 ? state.overrideState == 1 : config->enable;

    if (!wanted)
    {
        if (state.running)
        {
            StopRunning(state, config->workerDeadlineMs);
            LOG_INFO("playerbots.brain", "CoA brain off");
        }
        state.chatActive.store(false, std::memory_order_relaxed);
        return;
    }

    if (!BrainJson::Available())
    {
        state.lastProblem = "Boost.JSON is missing (Boost < 1.75)";
        if (!state.jsonMissingLogged)
        {
            LOG_ERROR("playerbots.brain", "CoA brain: {}; the brain stays off", state.lastProblem);
            state.jsonMissingLogged = true;
        }
        StopRunning(state, config->workerDeadlineMs);
        return;
    }

    BrainGovernor::Instance().Configure(*config);
    BrainCache::Instance().Configure(config->cacheSize, config->cacheTtlSeconds);

    if (state.running)
    {
        std::shared_ptr<BrainConfig const> const old = state.running->Config();
        if (old && old->workers == config->workers && old->caBundle == config->caBundle &&
            old->tlsVerify == config->tlsVerify)
        {
            state.running->Reconfigure(config);
            UpdateChatActive(state);
            LOG_INFO("playerbots.brain", "CoA brain reconfigured: chain {}, budget ${:.2f}/day", config->ChainText(),
                     double(config->maxUsdMicrosPerDay) / 1e6);
            return;
        }
        // Workers, CaBundle or TlsVerify changed: the clients are built at start, so the brain restarts.
        StopRunning(state, old ? old->workerDeadlineMs : config->workerDeadlineMs);
    }

    auto brain = std::make_shared<InProcessBrain>(config);
    std::string error;
    if (!brain->Start(error))
    {
        state.lastProblem = error;
        LOG_ERROR("playerbots.brain", "CoA brain could not start: {}", error);
        brain->Shutdown(0);
        state.chatActive.store(false, std::memory_order_relaxed);
        Install(state, nullptr);
        return;
    }

    state.running = brain;
    Install(state, brain);
    state.everStarted.store(true, std::memory_order_relaxed);
    UpdateChatActive(state);
    LOG_INFO("playerbots.brain", "CoA brain on: chain {}, workers {}, budget ${:.2f}/day", config->ChainText(),
             brain->WorkerCount(), double(config->maxUsdMicrosPerDay) / 1e6);
}

void SetOverride(int32 value)
{
    COA_ASSERT_WORLD_THREAD();
    State().overrideState = value < 0 ? -1 : (value ? 1 : 0);
}

int32 Override() { return State().overrideState; }

void ShutdownInitiate()
{
    COA_ASSERT_WORLD_THREAD();
    ServiceState& state = State();
    if (state.running)
        state.running->SetAccepting(false);
    UpdateChatActive(state);
}

void ShutdownCancel()
{
    COA_ASSERT_WORLD_THREAD();
    ServiceState& state = State();
    if (state.shutDown)
        return;
    if (state.running)
        state.running->SetAccepting(true);
    UpdateChatActive(state);
}

void Shutdown()
{
    COA_ASSERT_WORLD_THREAD();
    ServiceState& state = State();
    if (state.shutDown)
        return;
    state.shutDown = true;
    bool const wasRunning = state.running != nullptr;
    uint32 deadlineMs = 0;
    if (state.running)
        if (std::shared_ptr<BrainConfig const> const config = state.running->Config())
            deadlineMs = config->workerDeadlineMs;
    StopRunning(state, deadlineMs);
    if (wasRunning)
        LOG_INFO("playerbots.brain", "CoA brain off (shutdown)");
}

std::shared_ptr<InProcessBrain> Running()
{
    COA_ASSERT_WORLD_THREAD();
    return State().running;
}

std::string LastProblem()
{
    COA_ASSERT_WORLD_THREAD();
    return State().lastProblem;
}
}  // namespace BrainService
