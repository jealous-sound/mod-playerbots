/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainGovernor.h"
#include "BrainConfig.h"
#include "CoaTime.h"
#include "Log.h"
#include "StringFormat.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace
{
constexpr uint32 WINDOW = 20;               // the breaker also opens at WINDOW_FAILURES of the last WINDOW calls
constexpr uint32 WINDOW_FAILURES = 10;
constexpr uint32 WINDOW_MASK = (1u << WINDOW) - 1;
std::string const GLOBAL_ROW = "_global";
std::string const PROBE_ROW = "_probe";

bool HasCounters(BrainUsageRow const& row)
{
    return row.requests || row.failures || row.inputTokens || row.outputTokens || row.usdMicros || row.exhausted;
}

BrainUsageRow FreshRow(uint32 day, std::string const& engine)
{
    BrainUsageRow row;
    row.day = day;
    row.engine = engine;
    return row;
}

void CountCall(BrainUsageRow& row, BrainCallOutcome const& outcome)
{
    ++row.requests;
    if (outcome.error != BrainError::None)
        ++row.failures;
    row.inputTokens += outcome.usage.inputTokens;
    row.outputTokens += outcome.usage.outputTokens;
    row.usdMicros += outcome.usage.usdMicros;
}

bool BreakerFailure(BrainError error)
{
    switch (error)
    {
        case BrainError::Timeout:
        case BrainError::Network:
        case BrainError::Dns:
        case BrainError::ServerError:
        case BrainError::BadResponse:
            return true;
        default:
            return false;
    }
}

uint32 CountBits(uint32 bits)
{
    uint32 count = 0;
    for (; bits; bits &= bits - 1)
        ++count;
    return count;
}

bool Contains(std::vector<std::string> const& list, std::string_view item)
{
    return std::find(list.begin(), list.end(), item) != list.end();
}

std::string Excerpt(std::string_view detail)
{
    if (detail.empty())
        return "no detail";
    return std::string(detail.substr(0, 300));
}
}  // namespace

char const* BrainBreakerName(BrainBreakerState state)
{
    switch (state)
    {
        case BrainBreakerState::Closed:
            return "closed";
        case BrainBreakerState::Open:
            return "open";
        case BrainBreakerState::HalfOpen:
            return "half-open";
        default:
            return "unknown";
    }
}

BrainGovernor& BrainGovernor::Instance()
{
    static BrainGovernor instance;
    return instance;
}

void BrainGovernor::RolloverIfNeeded()
{
    uint32 const today = BrainClock::UtcDayNow();
    if (today == day)
        return;
    // A clock stepped back across midnight keeps counting on the later day: rolling back would reopen the
    // earlier day at zero and overwrite its stored totals (yyyymmdd compares in date order).
    if (day && today < day)
        return;

    if (day)
    {
        // The day that ended: its final totals are written once more (absolute values, so a repeat is harmless).
        for (EngineState& engine : engines)
            if (HasCounters(engine.today) || engine.dirty)
                finished.push_back(engine.today);
        if (HasCounters(global) || globalDirty)
            finished.push_back(global);
        if (HasCounters(probes) || probesDirty)
            finished.push_back(probes);
    }

    for (EngineState& engine : engines)
    {
        engine.today = FreshRow(today, engine.name);
        engine.dirty = false;
        if (engine.exhaustedDay != today)
            engine.exhaustedDay = 0;
    }
    global = FreshRow(today, GLOBAL_ROW);
    probes = FreshRow(today, PROBE_ROW);
    globalDirty = false;
    probesDirty = false;
    day = today;
}

BrainGovernor::EngineState* BrainGovernor::FindEngine(std::string_view name)
{
    for (EngineState& engine : engines)
        if (engine.name == name)
            return &engine;
    return nullptr;
}

BrainGovernor::EngineState const* BrainGovernor::FindEngine(std::string_view name) const
{
    for (EngineState const& engine : engines)
        if (engine.name == name)
            return &engine;
    return nullptr;
}

BrainError BrainGovernor::Check(EngineState const& engine, std::string_view situation, bool probe, uint64 nowMs) const
{
    if (!engine.configured || !engine.usable || engine.misconfigured)
        return BrainError::Misconfigured;
    if (engine.exhaustedDay && engine.exhaustedDay == day)
        return BrainError::Exhausted;
    if (engine.authFailed)
        return BrainError::AuthFailed;
    if (uint64(engine.today.requests) + (engine.textRow ? engine.textCallsInFlight : 0) >= engine.capRequests ||
        engine.today.usdMicros >= engine.capUsdMicros)
        return BrainError::Exhausted;
    if (probe)
        return BrainError::None;
    if (!situation.empty() && Contains(engine.disabledSituations, situation))
        return BrainError::BadRequest;

    bool const halfOpen = engine.breaker == BrainBreakerState::HalfOpen ||
                          (engine.breaker == BrainBreakerState::Open && engine.openUntilMs <= nowMs);
    if (engine.breaker == BrainBreakerState::Open && !halfOpen)
        return BrainError::NoEngine;
    if (halfOpen && engine.trialInFlight)
        return BrainError::NoEngine;
    return BrainError::None;
}

void BrainGovernor::Refill(uint64 nowMs)
{
    // Callers on other threads may pass a slightly older time: the refill clock only moves forward.
    if (nowMs <= lastRefillMs)
        return;
    tokens = TokensAt(nowMs);
    lastRefillMs = nowMs;
}

double BrainGovernor::TokensAt(uint64 nowMs) const
{
    double const rate = double(maxRequestsPerMinute) / 60000.0;
    double const elapsed = nowMs > lastRefillMs ? double(nowMs - lastRefillMs) : 0.0;
    return std::min(double(maxRequestsPerMinute), tokens + elapsed * rate);
}

void BrainGovernor::Configure(BrainConfig const& config)
{
    std::lock_guard<std::mutex> guard(lock);
    RolloverIfNeeded();

    capMicros = config.maxUsdMicrosPerDay;
    laneStopPercent = config.laneStopPercent;
    maxRequestsPerMinute = std::max<uint32>(1, config.maxRequestsPerMinute);
    breakerFailures = std::max<uint32>(1, config.breakerFailures);
    breakerOpenMs = config.breakerOpenSeconds * 1000;
    probeCap = config.probeMaxPerDay;
    connectTimeoutMs = config.connectTimeoutMs;

    std::vector<EngineState> ordered;
    ordered.reserve(engines.size() + config.engines.size());
    for (BrainEngineConfig const& engineConfig : config.engines)
    {
        auto const it = std::find_if(engines.begin(), engines.end(),
                                     [&engineConfig](EngineState const& e) { return e.name == engineConfig.name; });
        EngineState state;
        if (it != engines.end())
        {
            state = std::move(*it);
            engines.erase(it);
        }
        else
        {
            state.name = engineConfig.name;
            state.today = FreshRow(day, engineConfig.name);
        }
        state.textRow = false;
        state.configured = true;
        state.usable = engineConfig.usable;
        state.problem = engineConfig.problem;
        state.dialect = engineConfig.dialectName;
        state.capUsdMicros = engineConfig.maxUsdMicrosPerDay;
        state.capRequests = engineConfig.maxRequestsPerDay;
        // The new config may have fixed the key or the URL.
        state.authFailed = false;
        state.misconfigured = false;
        state.markStatus = 0;
        state.disabledSituations.clear();
        ordered.push_back(std::move(state));
    }
    if (config.text.provider != BrainTextProvider::None)
    {
        auto const it = std::find_if(engines.begin(), engines.end(),
                                    [](EngineState const& e) { return e.name == BRAIN_TEXT_ENGINE; });
        EngineState state;
        if (it != engines.end())
        {
            state = std::move(*it);
            engines.erase(it);
        }
        else
        {
            state.name = BRAIN_TEXT_ENGINE;
            state.today = FreshRow(day, state.name);
        }
        state.textRow = true;
        state.configured = true;
        state.usable = config.text.usable;
        state.problem = config.text.problem;
        state.dialect = config.text.providerName;
        state.capUsdMicros = config.text.maxUsdMicrosPerDay;
        state.capRequests = config.text.maxRequestsPerDay;
        state.authFailed = false;
        state.misconfigured = false;
        state.markStatus = 0;
        state.disabledSituations.clear();
        ordered.push_back(std::move(state));
    }
    for (EngineState& left : engines)
    {
        left.configured = false;
        ordered.push_back(std::move(left));
    }
    engines = std::move(ordered);

    uint64 const now = CoaTime::NowMs();
    if (!configuredOnce)
    {
        tokens = double(maxRequestsPerMinute);
        lastRefillMs = now;
        configuredOnce = true;
    }
    else
    {
        Refill(now);
        tokens = std::min(tokens, double(maxRequestsPerMinute));
    }
}

BrainSubmit BrainGovernor::Admit(BrainAdmitRequest const& request, uint64 nowMs)
{
    std::lock_guard<std::mutex> guard(lock);
    RolloverIfNeeded();
    Refill(nowMs);

    std::size_t const lane = std::min<std::size_t>(std::size_t(request.lane), laneStopPercent.size() - 1);
    uint64 const limit = capMicros * laneStopPercent[lane] / 100;
    if (global.usdMicros + reservedMicros + request.estimateMicros > limit)
        return BrainSubmit::OverBudget;
    EngineState const* text = request.textJob ? FindEngine(BRAIN_TEXT_ENGINE) : nullptr;
    if (request.textJob && text)
    {
        uint64 const textLimit = text->capUsdMicros * laneStopPercent[lane] / 100;
        if (text->today.usdMicros + textReservedMicros + request.estimateMicros > textLimit ||
            uint64(text->today.requests) + text->textCallsInFlight >= text->capRequests)
            return BrainSubmit::OverBudget;
    }
    if (request.probe && probes.requests + probesInFlight >= probeCap)
        return BrainSubmit::OverBudget;

    bool usable = false;
    if (request.textJob)
        usable = text && Check(*text, request.situation, request.probe, nowMs) == BrainError::None;
    else if (!request.forceEngine.empty())
    {
        EngineState const* engine = FindEngine(request.forceEngine);
        usable = engine && !engine->textRow && Check(*engine, request.situation, true, nowMs) == BrainError::None;
    }
    else
    {
        for (EngineState const& engine : engines)
        {
            if (engine.configured && !engine.textRow &&
                Check(engine, request.situation, request.probe, nowMs) == BrainError::None)
            {
                usable = true;
                break;
            }
        }
    }
    if (!usable)
        return BrainSubmit::NoEngine;

    if (tokens < 1.0)
        return BrainSubmit::Throttled;

    // Commit: nothing above changed any state.
    tokens -= 1.0;
    reservedMicros += request.estimateMicros;
    if (request.textJob)
        textReservedMicros += request.estimateMicros;
    if (request.probe)
        ++probesInFlight;
    return BrainSubmit::Queued;
}

void BrainGovernor::Settle(uint64 reserved, bool probe, bool textJob)
{
    std::lock_guard<std::mutex> guard(lock);
    reservedMicros -= std::min(reservedMicros, reserved);
    if (textJob)
        textReservedMicros -= std::min(textReservedMicros, reserved);
    if (probe && probesInFlight)
        --probesInFlight;
}

bool BrainGovernor::Acquire(std::string_view engineName, std::string_view situation, bool probe, uint64 nowMs,
                            uint32 timeLeftMs, BrainError& why)
{
    std::lock_guard<std::mutex> guard(lock);
    RolloverIfNeeded();

    EngineState* engine = FindEngine(engineName);
    if (!engine)
    {
        why = BrainError::NoEngine;
        return false;
    }
    BrainError const check = Check(*engine, situation, probe, nowMs);
    if (check != BrainError::None)
    {
        why = check;
        return false;
    }
    if (!probe && engine->latency.Count() >= BrainLatency::MinSamples && timeLeftMs < engine->latency.Percentile(50))
    {
        why = BrainError::TooLate;
        return false;
    }

    if (engine->textRow)
        ++engine->textCallsInFlight;
    if (!probe)
    {
        if (engine->breaker == BrainBreakerState::Open && engine->openUntilMs <= nowMs)
            engine->breaker = BrainBreakerState::HalfOpen;
        if (engine->breaker == BrainBreakerState::HalfOpen)
            engine->trialInFlight = true;
    }
    return true;
}

void BrainGovernor::Record(std::string_view engineName, std::string_view situation, BrainCallOutcome const& outcome,
                           uint64 nowMs)
{
    // Log lines are built under the lock and written after it.
    std::string warn;
    std::string error;
    {
        std::lock_guard<std::mutex> guard(lock);
        RolloverIfNeeded();

        EngineState* engine = FindEngine(engineName);
        if (!engine)
            return;
        if (engine->textRow && engine->textCallsInFlight)
            --engine->textCallsInFlight;
        if (!outcome.madeCall)
        {
            engine->trialInFlight = false;
            return;
        }

        CountCall(engine->today, outcome);
        engine->dirty = true;
        CountCall(global, outcome);
        globalDirty = true;
        if (outcome.probe)
        {
            CountCall(probes, outcome);
            probesDirty = true;
        }

        bool const wasOpen = engine->breaker == BrainBreakerState::Open && engine->openUntilMs > nowMs;
        bool const wasHalfOpen = engine->breaker == BrainBreakerState::HalfOpen ||
                                 (engine->breaker == BrainBreakerState::Open && engine->openUntilMs <= nowMs);

        if (outcome.error == BrainError::None)
        {
            engine->recentBits = (engine->recentBits << 1) & WINDOW_MASK;
            engine->recentCount = std::min(engine->recentCount + 1, WINDOW);
            engine->consecutiveFailures = 0;
            engine->breaker = BrainBreakerState::Closed;
            engine->openUntilMs = 0;
            engine->latency.Add(outcome.ms);
        }
        else if (BreakerFailure(outcome.error))
        {
            ++engine->consecutiveFailures;
            engine->recentBits = ((engine->recentBits << 1) | 1u) & WINDOW_MASK;
            engine->recentCount = std::min(engine->recentCount + 1, WINDOW);
            bool const windowFull = engine->recentCount >= WINDOW && CountBits(engine->recentBits) >= WINDOW_FAILURES;
            if (engine->consecutiveFailures >= breakerFailures || windowFull || wasHalfOpen)
            {
                engine->breaker = BrainBreakerState::Open;
                engine->openUntilMs = nowMs + breakerOpenMs;
                if (!wasOpen)
                    warn = Acore::StringFormat(
                        "CoA brain: engine {} is failing ({}, {} in a row); breaker open for {} s", engineName,
                        BrainErrorName(outcome.error), engine->consecutiveFailures, breakerOpenMs / 1000);
            }
        }
        else
        {
            switch (outcome.error)
            {
                case BrainError::Exhausted:
                    if (engine->exhaustedDay != day)
                        warn = Acore::StringFormat(
                            "CoA brain: engine {} is out of credit or rate limited (HTTP {}); off until the next UTC "
                            "day or .playerbots brain reset",
                            engineName, outcome.httpStatus);
                    engine->exhaustedDay = day;
                    engine->today.exhausted = true;
                    break;
                case BrainError::AuthFailed:
                    if (!engine->authFailed)
                        error = Acore::StringFormat(
                            "CoA brain: engine {} refused the API key (HTTP {}: {}); off until .reload config or "
                            ".playerbots brain reset",
                            engineName, outcome.httpStatus, Excerpt(outcome.detail));
                    engine->authFailed = true;
                    engine->markStatus = outcome.httpStatus;
                    break;
                case BrainError::Misconfigured:
                    if (!engine->misconfigured)
                        error = Acore::StringFormat(
                            "CoA brain: engine {} answered HTTP {} (wrong URL? try the alternates shown by "
                            ".playerbots brain probe); off until .reload config or .playerbots brain reset",
                            engineName, outcome.httpStatus);
                    engine->misconfigured = true;
                    engine->markStatus = outcome.httpStatus;
                    break;
                case BrainError::BadRequest:
                    if (!outcome.probe && !situation.empty() && !Contains(engine->disabledSituations, situation))
                    {
                        engine->disabledSituations.emplace_back(situation);
                        error = Acore::StringFormat(
                            "CoA brain: engine {} rejected a {} request (HTTP {}: {}); {} is off for it until "
                            ".reload config or .playerbots brain reset",
                            engineName, situation, outcome.httpStatus, Excerpt(outcome.detail), situation);
                    }
                    break;
                default:
                    break;
            }
        }
        engine->trialInFlight = false;
    }

    if (!warn.empty())
        LOG_WARN("playerbots.brain", "{}", warn);
    if (!error.empty())
        LOG_ERROR("playerbots.brain", "{}", error);
}

uint32 BrainGovernor::ExpectedMs(std::string_view forceEngine, uint64 nowMs) const
{
    std::lock_guard<std::mutex> guard(lock);
    // The singleton is never a const object: the day may roll over from here too.
    const_cast<BrainGovernor*>(this)->RolloverIfNeeded();

    EngineState const* chosen = nullptr;
    if (!forceEngine.empty())
    {
        EngineState const* engine = FindEngine(forceEngine);
        if (engine && !engine->textRow && Check(*engine, std::string_view(), true, nowMs) == BrainError::None)
            chosen = engine;
    }
    else
    {
        for (EngineState const& engine : engines)
        {
            if (engine.configured && !engine.textRow &&
                Check(engine, std::string_view(), false, nowMs) == BrainError::None)
            {
                chosen = &engine;
                break;
            }
        }
    }
    if (!chosen)
        return 0;
    if (chosen->latency.Count() >= BrainLatency::MinSamples)
        return chosen->latency.Percentile(95);
    return connectTimeoutMs / 2;
}

uint32 BrainGovernor::ExpectedTextMs(uint64 nowMs, uint32 fallbackMs) const
{
    std::lock_guard<std::mutex> guard(lock);
    const_cast<BrainGovernor*>(this)->RolloverIfNeeded();
    EngineState const* engine = FindEngine(BRAIN_TEXT_ENGINE);
    if (!engine || Check(*engine, std::string_view(), false, nowMs) != BrainError::None)
        return 0;
    return engine->latency.Count() >= BrainLatency::MinSamples ? engine->latency.Percentile(95) : fallbackMs;
}

std::vector<BrainUsageRow> BrainGovernor::TakeDirtyRows()
{
    std::lock_guard<std::mutex> guard(lock);
    RolloverIfNeeded();

    std::vector<BrainUsageRow> rows = std::move(finished);
    finished.clear();
    for (EngineState& engine : engines)
    {
        if (!engine.dirty)
            continue;
        rows.push_back(engine.today);
        engine.dirty = false;
    }
    if (globalDirty)
    {
        rows.push_back(global);
        globalDirty = false;
    }
    if (probesDirty)
    {
        rows.push_back(probes);
        probesDirty = false;
    }
    return rows;
}

void BrainGovernor::Seed(std::vector<BrainUsageRow> const& rows)
{
    std::lock_guard<std::mutex> guard(lock);
    RolloverIfNeeded();

    // Counters only grow during a day: the larger of the stored and the in-memory total wins.
    auto merge = [](BrainUsageRow& into, BrainUsageRow const& from)
    {
        into.requests = std::max(into.requests, from.requests);
        into.failures = std::max(into.failures, from.failures);
        into.inputTokens = std::max(into.inputTokens, from.inputTokens);
        into.outputTokens = std::max(into.outputTokens, from.outputTokens);
        into.usdMicros = std::max(into.usdMicros, from.usdMicros);
        into.exhausted = into.exhausted || from.exhausted;
    };

    for (BrainUsageRow const& row : rows)
    {
        if (row.day != day || row.engine.empty())
            continue;
        if (row.engine == GLOBAL_ROW)
        {
            merge(global, row);
            continue;
        }
        if (row.engine == PROBE_ROW)
        {
            merge(probes, row);
            continue;
        }
        EngineState* engine = FindEngine(row.engine);
        if (!engine)
        {
            EngineState state;
            state.name = row.engine;
            state.today = FreshRow(day, row.engine);
            engines.push_back(std::move(state));
            engine = &engines.back();
        }
        merge(engine->today, row);
        if (row.exhausted)
            engine->exhaustedDay = day;
    }
}

void BrainGovernor::ResetFlags()
{
    std::lock_guard<std::mutex> guard(lock);
    RolloverIfNeeded();

    for (EngineState& engine : engines)
    {
        engine.breaker = BrainBreakerState::Closed;
        engine.openUntilMs = 0;
        engine.trialInFlight = false;
        engine.consecutiveFailures = 0;
        engine.recentBits = 0;
        engine.recentCount = 0;
        engine.exhaustedDay = 0;
        if (engine.today.exhausted)
        {
            engine.today.exhausted = false;
            engine.dirty = true;
        }
        engine.authFailed = false;
        engine.misconfigured = false;
        engine.markStatus = 0;
        engine.disabledSituations.clear();
    }
}

std::vector<BrainEngineStatus> BrainGovernor::EngineStatus(uint64 nowMs) const
{
    std::lock_guard<std::mutex> guard(lock);
    const_cast<BrainGovernor*>(this)->RolloverIfNeeded();

    std::vector<BrainEngineStatus> out;
    for (EngineState const& engine : engines)
    {
        if (!engine.configured)
            continue;
        BrainEngineStatus status;
        status.name = engine.name;
        status.dialect = engine.dialect;
        status.usable = engine.usable;
        status.problem = engine.problem;
        status.breaker = engine.breaker;
        if (engine.breaker == BrainBreakerState::Open)
        {
            if (engine.openUntilMs > nowMs)
                status.openForMs =
                    uint32(std::min<uint64>(engine.openUntilMs - nowMs, std::numeric_limits<uint32>::max()));
            else
                status.breaker = BrainBreakerState::HalfOpen;
        }
        status.exhausted = engine.exhaustedDay && engine.exhaustedDay == day;
        status.authFailed = engine.authFailed;
        status.misconfigured = engine.misconfigured;
        status.markStatus = engine.markStatus;
        status.disabledSituations = engine.disabledSituations;
        status.today = engine.today;
        status.capUsdMicros = engine.capUsdMicros;
        status.capRequests = engine.capRequests;
        status.samples = engine.latency.Count();
        status.p50Ms = engine.latency.Percentile(50);
        status.p95Ms = engine.latency.Percentile(95);
        out.push_back(std::move(status));
    }
    return out;
}

BrainBudgetStatus BrainGovernor::Budget(uint64 nowMs) const
{
    std::lock_guard<std::mutex> guard(lock);
    const_cast<BrainGovernor*>(this)->RolloverIfNeeded();

    BrainBudgetStatus budget;
    budget.day = day;
    budget.spentMicros = global.usdMicros;
    budget.reservedMicros = reservedMicros;
    budget.capMicros = capMicros;
    budget.requests = global.requests;
    budget.probes = probes.requests;
    budget.probeCap = probeCap;
    budget.tokens = configuredOnce ? uint32(TokensAt(nowMs)) : 0;
    return budget;
}
