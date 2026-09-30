/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InProcessBrain.h"
#include "BrainCache.h"
#include "BrainGovernor.h"
#include "BrainHttpClient.h"
#include "BrainJson.h"
#include "BrainQueue.h"
#include "BrainResolver.h"
#include "BrainSink.h"
#include "BrainStats.h"
#include "CoaText.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "IDecisionEngine.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

struct InProcessBrain::Shared
{
    // Immutable once published; a Reconfigure publishes a new one and calls in flight keep theirs.
    struct EngineSet
    {
        std::shared_ptr<BrainConfig const> config;
        std::vector<std::shared_ptr<IDecisionEngine>> chain;  // usable engines, Decision.Chain order
        std::unordered_map<std::string, std::shared_ptr<IDecisionEngine>> byName;
        bool anyCalibrated = false;
        std::string chainSignature;  // "name:model:c|u,..." (c = calibrated) appended to every cache key
    };

    BrainQueue queue;
    mutable std::mutex setLock;
    std::shared_ptr<EngineSet const> set;  // guarded by setLock
    std::vector<std::shared_ptr<BrainHttpClient>> clients;  // one per worker; filled before the workers start
    std::atomic<bool> stopping{false};
    std::mutex exitLock;
    std::condition_variable exited;
    uint32 alive = 0;        // guarded by exitLock
    std::vector<bool> done;  // guarded by exitLock: worker i left its loop

    std::shared_ptr<EngineSet const> Set() const
    {
        std::lock_guard<std::mutex> guard(setLock);
        return set;
    }
};

namespace
{
constexpr uint64 DEFAULT_DEADLINE_MS = 10000;
constexpr uint32 MIN_CALL_BUDGET_MS = 100;
constexpr uint32 DEADLINE_MARGIN_MS = 50;
constexpr uint32 IDLE_CONNECTION_MS = 30000;
constexpr uint32 TOO_LATE_PROBE_EVERY = 32;
constexpr std::size_t DETAIL_BYTES = 300;

// Process-wide, so a brain started after a restart never reuses an id: a late Cancel(id) aimed at the old brain's
// job then matches nothing in the new one.
std::atomic<uint64> NextJobId{1};

uint32 Clamp32(uint64 value) { return uint32(std::min<uint64>(value, std::numeric_limits<uint32>::max())); }

bool Cacheable(BrainJob const& job)
{
    return job.use == BrainUse::ChatReply && !job.cacheKey.empty() && job.forceEngine.empty();
}

bool MadeCall(BrainError error)
{
    switch (error)
    {
        case BrainError::Dns:
        case BrainError::Cancelled:
        case BrainError::NotBuilt:
        case BrainError::NoEngine:
        case BrainError::TooLate:
            return false;
        default:
            return true;
    }
}

bool ValidQuestion(BrainQuestion const& question)
{
    if (question.name.empty())
        return false;
    switch (question.type)
    {
        case BrainQuestionType::Choice:
            return !question.options.empty() && question.options.size() <= 255;
        case BrainQuestionType::Score:
            return question.options.size() >= 2 && question.options.size() <= 10;
        case BrainQuestionType::YesNo:
        {
            if (question.options.empty())
                return true;
            if (question.options.size() != 2)
                return false;
            std::string const& a = question.options[0].key;
            std::string const& b = question.options[1].key;
            return (a == "true" && b == "false") || (a == "false" && b == "true");
        }
        default:
            return false;
    }
}

std::vector<std::string> HostsOf(BrainConfig const& config)
{
    std::vector<std::string> hosts;
    for (BrainEngineConfig const& engine : config.engines)
    {
        // Mock has no host; IP literals and localhost need no lookup (JevEngine maps localhost itself).
        if (!engine.usable || engine.host.empty() || engine.hostIsIp || engine.host == "localhost")
            continue;
        if (std::find(hosts.begin(), hosts.end(), engine.host) == hosts.end())
            hosts.push_back(engine.host);
    }
    return hosts;
}

// Reserved from the budget until the job ends: the prompt's bytes as tokens (3.5 bytes each) at the dearest
// input price of the engines that may take it, or a mock's fixed price per call; at least 1 micro-USD.
uint64 Estimate(BrainJob const& job, std::vector<IDecisionEngine const*> const& engines)
{
    std::size_t bytes = job.state.size() + job.playerText.size();
    for (BrainQuestion const& question : job.questions)
    {
        bytes += question.instructions.size();
        for (BrainOption const& option : question.options)
            bytes += option.text.size();
    }
    double const tokens = double(bytes) / 3.5;
    uint64 worst = 0;
    for (IDecisionEngine const* engine : engines)
    {
        BrainEngineConfig const& config = engine->Config();
        uint64 const micros = uint64(std::ceil(tokens * config.priceInPerMTok));
        worst = std::max({worst, micros, config.mockUsdMicrosPerCall});
    }
    return worst + 1;
}

std::string Excerpt(std::string text)
{
    CoaText::CutUtf8Bytes(text, DETAIL_BYTES);
    return text;
}
}  // namespace

InProcessBrain::InProcessBrain(std::shared_ptr<BrainConfig const> config) : shared(std::make_shared<Shared>())
{
    auto set = std::make_shared<Shared::EngineSet>();
    set->config = config ? std::move(config) : std::make_shared<BrainConfig const>();
    shared->set = std::move(set);
}

InProcessBrain::~InProcessBrain()
{
    if (!stopped)
        Shutdown(0);
}

bool InProcessBrain::InstallEngines(std::shared_ptr<BrainConfig const> const& config, std::string& problems)
{
    auto set = std::make_shared<Shared::EngineSet>();
    set->config = config;
    auto note = [&problems](std::string const& name, std::string const& why)
    {
        if (!problems.empty())
            problems += "; ";
        problems += name + ": " + why;
    };

    for (BrainEngineConfig const& engineConfig : config->engines)
    {
        if (!engineConfig.usable)
        {
            note(engineConfig.name, engineConfig.problem);
            continue;
        }
        std::string error;
        std::shared_ptr<IDecisionEngine> engine;
        try
        {
            engine = BrainEngines::Create(engineConfig, *config, resolver, error);
        }
        catch (std::exception const& e)
        {
            error = e.what();
        }
        catch (...)
        {
            error = "unknown exception";
        }
        if (!engine)
        {
            LOG_ERROR("playerbots.brain", "CoA brain: engine {} could not be built: {}", engineConfig.name, error);
            note(engineConfig.name, error);
            continue;
        }
        set->chain.push_back(engine);
        set->byName.emplace(engineConfig.name, engine);
        set->anyCalibrated = set->anyCalibrated || engine->Calibrated();
        if (!set->chainSignature.empty())
            set->chainSignature += ',';
        // The calibration is part of the key: a cached calibrated answer must not decide once the engine is
        // declared uncalibrated (and the reverse).
        set->chainSignature += engineConfig.name + ":" + engineConfig.model + (engine->Calibrated() ? ":c" : ":u");
    }

    bool const any = !set->chain.empty();
    useChatReply.store(config->useChatReply, std::memory_order_relaxed);
    chainCalibrated.store(set->anyCalibrated, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(shared->setLock);
        shared->set = std::move(set);
    }
    return any;
}

bool InProcessBrain::Start(std::string& error)
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped || running.load(std::memory_order_relaxed))
    {
        error = stopped ? "this brain was shut down" : "already running";
        return false;
    }
    if (!BrainJson::Available())
    {
        error = "Boost.JSON is missing (Boost < 1.75)";
        return false;
    }

    std::shared_ptr<BrainConfig const> const config = shared->Set()->config;
    std::vector<std::string> hosts = HostsOf(*config);
    if (!hosts.empty())
        resolver = BrainResolver::Start(std::move(hosts), config->dnsRefreshSeconds);

    auto fail = [this, &error](std::string why)
    {
        error = std::move(why);
        shared->queue.Close();
        if (resolver)
        {
            resolver->Stop();
            resolver.reset();
        }
        return false;
    };

    std::string problems;
    if (!InstallEngines(config, problems))
        return fail("no usable engine: " + (problems.empty() ? std::string("the chain is empty") : problems));

    uint32 const count = std::max<uint32>(1, config->workers);
    BrainTlsSettings tls;
    tls.verify = config->tlsVerify;
    tls.caBundle = BrainConfig::ResolvePath(config->caBundle);
    try
    {
        shared->clients.clear();
        for (uint32 i = 0; i < count; ++i)
            shared->clients.push_back(std::make_shared<BrainHttpClient>(tls));
    }
    catch (std::exception const& e)
    {
        return fail(std::string("HTTP client: ") + e.what());
    }
    catch (...)
    {
        return fail("HTTP client: unknown exception");
    }

    bool anyTls = false;
    for (std::shared_ptr<IDecisionEngine> const& engine : shared->Set()->chain)
        anyTls = anyTls || (engine->Config().tls && !engine->Config().host.empty());
    if (anyTls && !shared->clients.front()->TlsReady())
        LOG_ERROR("playerbots.brain", "CoA brain: TLS is not ready ({}); https engines will fail until it is fixed",
                  shared->clients.front()->TlsProblem());
    if (!config->tlsVerify)
        LOG_WARN("playerbots.brain",
                 "CoA brain: certificate checks are off (AiPlayerbot.CoaBrain.TlsVerify = 0); anyone on the path can "
                 "read and forge the engines' traffic");

    {
        std::lock_guard<std::mutex> guard(shared->exitLock);
        shared->done.assign(count, false);
        shared->alive = count;
    }
    workers.reserve(count);
    for (uint32 i = 0; i < count; ++i)
    {
        try
        {
            workers.emplace_back([worker = shared, i] { Work(worker, i); });
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("playerbots.brain", "CoA brain: worker {} could not start: {}", i, e.what());
            std::lock_guard<std::mutex> guard(shared->exitLock);
            for (uint32 j = i; j < count; ++j)
                shared->done[j] = true;
            shared->alive -= count - i;
            break;
        }
    }
    if (workers.empty())
        return fail("no worker thread could start");

    running.store(true, std::memory_order_relaxed);
    accepting.store(true, std::memory_order_relaxed);
    return true;
}

void InProcessBrain::Reconfigure(std::shared_ptr<BrainConfig const> config)
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped || !config)
        return;

    std::vector<std::string> hosts = HostsOf(*config);
    if (resolver)
        resolver->SetHosts(std::move(hosts), config->dnsRefreshSeconds);
    else if (!hosts.empty())
        resolver = BrainResolver::Start(std::move(hosts), config->dnsRefreshSeconds);

    std::string problems;
    if (!InstallEngines(config, problems))
        LOG_ERROR("playerbots.brain",
                  "CoA brain: no usable engine after the reload ({}); every decision falls back to the heuristic",
                  problems.empty() ? std::string("the chain is empty") : problems);
}

void InProcessBrain::SetAccepting(bool value)
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped)
        return;
    accepting.store(value, std::memory_order_relaxed);
}

void InProcessBrain::Shutdown(uint32 deadlineMs)
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped)
        return;
    stopped = true;
    accepting.store(false, std::memory_order_relaxed);
    running.store(false, std::memory_order_relaxed);
    shared->stopping.store(true, std::memory_order_release);

    // Waiting jobs are dropped: nothing is posted for them, their reservations go back.
    BrainGovernor& governor = BrainGovernor::Instance();
    for (BrainQueued const& item : shared->queue.Close())
        governor.Settle(item.reservedMicros, item.job.use == BrainUse::Probe);
    for (std::shared_ptr<BrainHttpClient> const& client : shared->clients)
        if (client)
            client->Interrupt();

    {
        std::unique_lock<std::mutex> guard(shared->exitLock);
        shared->exited.wait_for(guard, std::chrono::milliseconds(deadlineMs), [this] { return shared->alive == 0; });
    }

    // A worker still inside a call (getaddrinfo, a TLS read) is detached: it holds its Shared block and leaves on
    // its own, posting nothing since `stopping` is set.
    uint32 detached = 0;
    for (std::size_t i = 0; i < workers.size(); ++i)
    {
        std::thread& worker = workers[i];
        if (!worker.joinable())
            continue;
        bool finished = false;
        {
            std::lock_guard<std::mutex> guard(shared->exitLock);
            finished = i < shared->done.size() && shared->done[i];
        }
        if (finished)
            worker.join();
        else
        {
            worker.detach();
            ++detached;
        }
    }
    workers.clear();
    if (detached)
        LOG_DEBUG("playerbots.brain", "CoA brain: {} worker(s) still in a call were detached at shutdown", detached);

    if (resolver)
    {
        resolver->Stop();
        resolver.reset();
    }
}

bool InProcessBrain::Active(BrainUse use) const
{
    return running.load(std::memory_order_relaxed) && accepting.load(std::memory_order_relaxed) &&
           (use != BrainUse::ChatReply || useChatReply.load(std::memory_order_relaxed));
}

bool InProcessBrain::ChainCalibrated() const { return chainCalibrated.load(std::memory_order_relaxed); }

BrainTicket InProcessBrain::Submit(BrainJob&& job, std::shared_ptr<BrainSink> const& sink, BrainResult* inlineHit)
{
    BrainStats& stats = BrainStats::Instance();
    auto refuse = [&stats](BrainSubmit status)
    {
        stats.Count(status);
        BrainTicket ticket;
        ticket.status = status;
        return ticket;
    };

    // Read-only checks first; nothing is reserved before Admit, which checks everything then commits.
    if (!Active(job.use))
        return refuse(BrainSubmit::Off);

    std::shared_ptr<Shared::EngineSet const> const set = shared->Set();
    bool valid = sink && !job.questions.empty();
    for (BrainQuestion const& question : job.questions)
        valid = valid && ValidQuestion(question);
    if (valid && !job.forceEngine.empty() && !set->byName.contains(job.forceEngine))
        valid = false;
    if (!valid)
        return refuse(BrainSubmit::Invalid);

    uint64 const now = CoaTime::NowMs();
    job.id = NextJobId.fetch_add(1, std::memory_order_relaxed);
    job.submitMs = now;
    if (!job.deadlineMs)
        job.deadlineMs = now + DEFAULT_DEADLINE_MS;

    if (Cacheable(job))
    {
        BrainResult hit;
        if (BrainCache::Instance().Lookup(job.cacheKey + "|" + set->chainSignature, now, hit))
        {
            hit.jobId = job.id;
            hit.tag = job.tag;
            hit.epoch = job.epoch;
            hit.use = job.use;
            if (inlineHit)
                *inlineHit = std::move(hit);
            stats.Count(BrainSubmit::Answered);
            BrainTicket ticket;
            ticket.status = BrainSubmit::Answered;
            ticket.jobId = job.id;
            return ticket;
        }
    }

    BrainGovernor& governor = BrainGovernor::Instance();
    uint64 const timeLeft = job.deadlineMs > now ? job.deadlineMs - now : 0;
    if (timeLeft < MIN_CALL_BUDGET_MS + DEADLINE_MARGIN_MS)
        return refuse(BrainSubmit::TooLate);
    // One refusal in 32 goes through anyway: the latency figures only change with calls, so an engine judged too
    // slow once would otherwise never be measured again.
    if (timeLeft < governor.ExpectedMs(job.forceEngine, now) &&
        tooLateSkips.fetch_add(1, std::memory_order_relaxed) % TOO_LATE_PROBE_EVERY != 0)
        return refuse(BrainSubmit::TooLate);

    if (!shared->queue.CanPush(job.lane))
        return refuse(BrainSubmit::Full);

    std::vector<IDecisionEngine const*> candidates;
    if (!job.forceEngine.empty())
        candidates.push_back(set->byName.at(job.forceEngine).get());
    else
        for (std::shared_ptr<IDecisionEngine> const& engine : set->chain)
            candidates.push_back(engine.get());
    uint64 const estimate = Estimate(job, candidates);

    bool const probe = job.use == BrainUse::Probe;
    BrainAdmitRequest request;
    request.lane = job.lane;
    request.probe = probe;
    request.situation = job.situation;
    request.forceEngine = job.forceEngine;
    request.estimateMicros = estimate;
    BrainSubmit const admitted = governor.Admit(request, now);
    if (admitted != BrainSubmit::Queued)
        return refuse(admitted);

    uint64 const id = job.id;
    BrainQueued item;
    item.job = std::move(job);
    item.sink = sink;
    item.reservedMicros = estimate;
    item.enqueuedMs = now;
    std::vector<BrainQueued> evicted;
    if (!shared->queue.Push(std::move(item), evicted))
    {
        governor.Settle(estimate, probe);
        return refuse(BrainSubmit::Full);
    }

    for (BrainQueued& old : evicted)
    {
        governor.Settle(old.reservedMicros, old.job.use == BrainUse::Probe);
        BrainResult result;
        result.jobId = old.job.id;
        result.tag = old.job.tag;
        result.epoch = old.job.epoch;
        result.use = old.job.use;
        result.error = BrainError::Cancelled;
        result.detail = "evicted from the ambient lane";
        stats.Count(BrainError::Cancelled);
        if (old.sink)
            old.sink->Post(std::move(result));
    }

    stats.Count(BrainSubmit::Queued);
    BrainTicket ticket;
    ticket.status = BrainSubmit::Queued;
    ticket.jobId = id;
    return ticket;
}

void InProcessBrain::Cancel(uint64 jobId)
{
    BrainQueued removed;
    if (shared->queue.Remove(jobId, removed))
        BrainGovernor::Instance().Settle(removed.reservedMicros, removed.job.use == BrainUse::Probe);
}

std::shared_ptr<BrainConfig const> InProcessBrain::Config() const { return shared->Set()->config; }

uint32 InProcessBrain::QueueDepth(BrainLane lane) const { return shared->queue.Depth(lane); }

void InProcessBrain::Work(std::shared_ptr<Shared> shared, uint32 index)
{
    BrainStats& stats = BrainStats::Instance();
    BrainGovernor& governor = BrainGovernor::Instance();

    try
    {
        BrainQueued item;
        while (shared->queue.Pop(item))
        {
            stats.inFlight.fetch_add(1, std::memory_order_relaxed);
            BrainJob const& job = item.job;
            bool const probe = job.use == BrainUse::Probe;
            uint64 const start = CoaTime::NowMs();

            BrainResult result;
            result.jobId = job.id;
            result.tag = job.tag;
            result.epoch = job.epoch;
            result.use = job.use;
            result.queuedMs = Clamp32(start > job.submitMs ? start - job.submitMs : 0);

            // A throw becomes an error result: std::terminate in a worker would take the realm down.
            try
            {
                std::shared_ptr<Shared::EngineSet const> const set = shared->Set();
                BrainHttpClient& http = *shared->clients[index];
                BrainError lastError = BrainError::NoEngine;

                if (start >= job.deadlineMs)
                    lastError = BrainError::TooLate;
                else
                {
                    std::vector<std::shared_ptr<IDecisionEngine>> forced;
                    if (!job.forceEngine.empty())
                    {
                        auto const it = set->byName.find(job.forceEngine);
                        if (it != set->byName.end())
                            forced.push_back(it->second);
                    }
                    std::vector<std::shared_ptr<IDecisionEngine>> const& engines =
                        job.forceEngine.empty() ? set->chain : forced;

                    for (std::shared_ptr<IDecisionEngine> const& engine : engines)
                    {
                        uint64 const now = CoaTime::NowMs();
                        if (now >= job.deadlineMs)
                        {
                            lastError = BrainError::TooLate;
                            break;
                        }
                        uint32 const timeLeft = Clamp32(job.deadlineMs - now);
                        std::string const& name = engine->Config().name;

                        BrainError why = BrainError::NoEngine;
                        if (!governor.Acquire(name, job.situation, probe, now, timeLeft, why))
                        {
                            lastError = why;
                            continue;
                        }

                        uint32 const budget = std::min(set->config->readTimeoutMs,
                                                       timeLeft - std::min(timeLeft, DEADLINE_MARGIN_MS));
                        if (budget < MIN_CALL_BUDGET_MS)
                        {
                            BrainCallOutcome skipped;
                            skipped.error = BrainError::TooLate;
                            skipped.probe = probe;
                            skipped.madeCall = false;
                            governor.Record(name, job.situation, skipped, now);
                            lastError = BrainError::TooLate;
                            break;
                        }

                        DecisionCall call;
                        call.job = &job;
                        call.sendPlayerText = engine->Config().sendPlayerText;
                        call.stateMaxChars = set->config->stateMaxChars;
                        call.connectTimeoutMs = set->config->connectTimeoutMs;
                        call.record = job.record;
                        call.cancel = &shared->stopping;
                        DecisionReply reply = engine->Decide(call, http, budget);

                        BrainCallOutcome outcome;
                        outcome.error = reply.error;
                        outcome.httpStatus = reply.httpStatus;
                        outcome.usage = reply.usage;
                        outcome.ms = reply.ms;
                        outcome.probe = probe;
                        outcome.madeCall = MadeCall(reply.error);
                        outcome.detail = reply.detail;
                        governor.Record(name, job.situation, outcome, CoaTime::NowMs());

                        ++result.enginesTried;
                        result.engine = name;
                        result.httpStatus = reply.httpStatus;
                        result.detail = Excerpt(std::move(reply.detail));
                        if (job.record)
                        {
                            result.url = std::move(reply.url);
                            result.rawRequest = std::move(reply.rawRequest);
                            result.rawResponse = std::move(reply.rawResponse);
                        }
                        if (reply.error == BrainError::None)
                        {
                            result.answers = std::move(reply.answers);
                            result.usage = reply.usage;
                            result.providerModel = std::move(reply.providerModel);
                            result.calibrated = engine->Calibrated();
                            lastError = BrainError::None;
                            break;
                        }
                        lastError = reply.error;
                        if (reply.error == BrainError::Cancelled)
                            break;
                    }
                }

                result.error = lastError;
                if (result.error == BrainError::None && Cacheable(job))
                    BrainCache::Instance().Store(job.cacheKey + "|" + set->chainSignature, job, result,
                                                 CoaTime::NowMs());
            }
            catch (std::exception const& e)
            {
                result.error = BrainError::Network;
                result.answers.clear();
                result.detail = Excerpt(std::string("exception: ") + e.what());
            }
            catch (...)
            {
                result.error = BrainError::Network;
                result.answers.clear();
                result.detail = "exception";
            }

            if (result.error != BrainError::None)
                result.answers.clear();
            uint64 const end = CoaTime::NowMs();
            result.callMs = Clamp32(end > start ? end - start : 0);
            stats.Count(result.error);
            LOG_DEBUG("playerbots.brain", "CoA brain: job {} ({}, tag {}): {} via {} in {} ms after {} ms queued",
                      result.jobId, BrainUseName(result.use), result.tag, BrainErrorName(result.error),
                      result.engine.empty() ? std::string("-") : result.engine, result.callMs, result.queuedMs);

            governor.Settle(item.reservedMicros, probe);
            if (!shared->stopping.load(std::memory_order_acquire) && item.sink)
                item.sink->Post(std::move(result));
            stats.inFlight.fetch_sub(1, std::memory_order_relaxed);
            shared->clients[index]->CloseIdle(IDLE_CONNECTION_MS);
            item = BrainQueued{};
        }
    }
    catch (...)
    {
        // Only the queue's own locking can land here; the worker leaves and the others carry on.
    }

    {
        std::lock_guard<std::mutex> guard(shared->exitLock);
        if (index < shared->done.size())
            shared->done[index] = true;
        if (shared->alive)
            --shared->alive;
    }
    shared->exited.notify_all();
}
