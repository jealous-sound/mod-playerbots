/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINGOVERNOR_H
#define PLAYERBOTS_BRAINGOVERNOR_H

#include "BrainLatency.h"
#include "BrainTypes.h"
#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct BrainConfig;

enum class BrainBreakerState : uint8
{
    Closed = 0,  // calls go through
    Open,        // Breaker.Failures in a row (or half of the last 20) failed: skipped for Breaker.OpenSeconds
    HalfOpen     // the open time passed: one trial call; success closes, failure opens again
};

char const* BrainBreakerName(BrainBreakerState state);  // "closed", "open", "half-open"

// One row of playerbots_coa_brain_usage: absolute totals for one UTC day.
struct BrainUsageRow
{
    uint32 day = 0;       // yyyymmdd
    std::string engine;   // an engine name, "_global" (every call) or "_probe" (probe calls only)
    uint32 requests = 0;  // HTTP calls made (a call that never left: Dns, NoEngine, TooLate, is not one)
    uint32 failures = 0;  // calls that did not return usable answers
    uint64 inputTokens = 0;
    uint64 outputTokens = 0;
    uint64 usdMicros = 0;
    bool exhausted = false;  // a 402, 429 or spend-limit error was seen that day
};

struct BrainEngineStatus
{
    std::string name;
    std::string dialect;
    bool usable = false;
    std::string problem;  // config problem (no key, bad URL, not built)
    BrainBreakerState breaker = BrainBreakerState::Closed;
    uint32 openForMs = 0;
    bool exhausted = false;      // until the next UTC day
    bool authFailed = false;     // until .reload config or reset
    bool misconfigured = false;  // HTTP 404: until .reload config or reset
    int32 markStatus = 0;        // the HTTP status that set the auth-failed or misconfigured mark
    std::vector<std::string> disabledSituations;  // HTTP 400/413/422
    BrainUsageRow today;
    uint64 capUsdMicros = 0;
    uint32 capRequests = 0;
    uint32 p50Ms = 0;
    uint32 p95Ms = 0;
    uint32 samples = 0;
};

struct BrainBudgetStatus
{
    uint32 day = 0;
    uint64 spentMicros = 0;     // the "_global" row
    uint64 reservedMicros = 0;  // estimates of the jobs in flight
    uint64 capMicros = 0;       // MaxUsdPerDay
    uint32 requests = 0;
    uint32 probes = 0;
    uint32 probeCap = 0;
    uint32 tokens = 0;  // what is left of the MaxRequestsPerMinute bucket
};

// What Submit asks for.
struct BrainAdmitRequest
{
    BrainLane lane = BrainLane::Player;
    bool probe = false;
    std::string_view situation;    // an engine with this situation disabled does not count as usable
    std::string_view forceEngine;  // probe: only that engine is considered (its breaker is ignored)
    uint64 estimateMicros = 0;     // reserved from the global budget until Settle
    // M6. A text job: only the engine "text" is considered (never a chain engine), and the lane's watermark also
    // applies to the text row's own daily cap (Text.MaxUsdPerDay x LaneStopAt[lane] / 100, reservations included),
    // so the Ambient lane (banter) stops at half of the text budget and leaves the rest to players' replies.
    bool textJob = false;
};

// What one HTTP call (or one attempt that never left) came to.
struct BrainCallOutcome
{
    BrainError error = BrainError::None;
    int32 httpStatus = 0;
    BrainUsage usage;
    uint32 ms = 0;
    bool probe = false;
    bool madeCall = true;  // false for Dns, NoEngine, TooLate, Cancelled before sending: nothing is counted
    std::string_view detail;  // the error excerpt, logged once with the mark it sets
};

// Budgets, breakers and rate for every engine, process-wide: its state outlives brain restarts and reloads, so
// the day's spending and open breakers survive .playerbots brain off/on. Thread-safe; one mutex, held for a few
// map lookups. Admission is check-then-commit: Admit checks everything, then commits the rate token and the
// reservation together, so a refused job costs nothing.
class BrainGovernor
{
public:
    static BrainGovernor& Instance();

    // World thread (BrainService::Apply: every config load, .playerbots brain on/off). Caps and the chain order.
    // An engine that stays keeps its counters, breaker and exhausted mark, and loses its auth-failed,
    // misconfigured and disabled-situation marks (the new config may have fixed the key or the URL); a new one
    // starts clean; one that left keeps its row until the day ends. M6: when config.text.provider is not None the
    // row BRAIN_TEXT_ENGINE ("text") is configured like an engine (caps from Text.MaxUsdPerDay and
    // Text.MaxRequestsPerDay, dialect = the provider's name) and listed after the chain; it is flagged as the text
    // row, which ExpectedMs and the decision jobs' Admit never consider.
    void Configure(BrainConfig const& config);

    // Any thread (Submit). In order: global budget with the lane's watermark (spent + reserved + estimate <=
    // cap x LaneStopAt[lane] / 100) -> OverBudget; for a text job the text row's daily cap with the same watermark
    // and its request cap -> OverBudget; probe cap (probes) -> OverBudget; at least one engine usable (forceEngine:
    // that one, breaker ignored; a text job: the text row) -> NoEngine; the MaxRequestsPerMinute bucket ->
    // Throttled. When all pass, takes one token, reserves the estimate and returns Queued.
    BrainSubmit Admit(BrainAdmitRequest const& request, uint64 nowMs);

    // Any thread. Exactly once per admitted job, whatever its end (answered, failed, cancelled, evicted, dropped at
    // shutdown, refused by the queue after Admit): returns the reservation and, for a probe, its in-flight slot.
    // `textJob`: the job was admitted with BrainAdmitRequest::textJob (its reservation also sits on the text row).
    void Settle(uint64 reservedMicros, bool probe, bool textJob = false);

    // Worker thread, before each engine of the chain (the text worker: the engine BRAIN_TEXT_ENGINE). False, with
    // `why`, when the engine is not usable, is
    // exhausted, auth-failed or misconfigured, has `situation` disabled, is past its own daily caps, has an
    // open breaker (a half-open one lets one trial through), or when timeLeftMs is under its p50. A probe
    // ignores the breaker, the disabled situations and the p50.
    bool Acquire(std::string_view engine, std::string_view situation, bool probe, uint64 nowMs, uint32 timeLeftMs,
                 BrainError& why);

    // Worker thread, after each attempt. Counts the call on the engine's, "_global" and (probes) "_probe"
    // rows; updates the breaker (Timeout, Network, Dns, ServerError, BadResponse fail; None closes); marks
    // Exhausted (until the next UTC day), AuthFailed, Misconfigured, or disables `situation` (BadRequest),
    // logging each mark once; adds `ms` to the latency ring on success.
    void Record(std::string_view engine, std::string_view situation, BrainCallOutcome const& outcome, uint64 nowMs);

    // Any thread (Submit's TooLate test). The p95 of the first usable engine of the chain (or of forceEngine),
    // ConnectTimeoutMs / 2 while it has fewer than BrainLatency::MinSamples samples. The text row is never a
    // candidate here.
    uint32 ExpectedMs(std::string_view forceEngine, uint64 nowMs) const;

    // M6. Any thread (Submit's TooLate test for text jobs). The p95 of the text row when it is usable, else 0; while
    // it has fewer than BrainLatency::MinSamples samples, `fallbackMs` (Submit passes Text.TimeoutMs / 3).
    uint32 ExpectedTextMs(uint64 nowMs, uint32 fallbackMs) const;

    // World thread (BrainUsageStore). Rows changed since the last call: today's, and the final rows of the day
    // that just ended. The caller writes them; they are not returned again unless they change.
    std::vector<BrainUsageRow> TakeDirtyRows();

    // World thread (BrainUsageStore::LoadSync, before the first start). Today's persisted totals: counters and
    // exhausted marks start from them. Rows of another day are ignored.
    void Seed(std::vector<BrainUsageRow> const& rows);

    // World thread (.playerbots brain reset). Closes every breaker and clears the exhausted, auth-failed,
    // misconfigured and disabled-situation marks. Counters stay.
    void ResetFlags();

    // World thread (.playerbots brain status, bot-status.json).
    std::vector<BrainEngineStatus> EngineStatus(uint64 nowMs) const;
    BrainBudgetStatus Budget(uint64 nowMs) const;

private:
    BrainGovernor() = default;
    BrainGovernor(BrainGovernor const&) = delete;
    BrainGovernor& operator=(BrainGovernor const&) = delete;

    struct EngineState
    {
        std::string name;
        std::string dialect;
        bool configured = false;  // in the current chain
        bool usable = false;
        std::string problem;
        uint64 capUsdMicros = 0;
        uint32 capRequests = 0;
        BrainBreakerState breaker = BrainBreakerState::Closed;
        uint32 consecutiveFailures = 0;
        uint32 recentBits = 0;   // the last 20 outcomes, 1 = failure
        uint32 recentCount = 0;
        uint64 openUntilMs = 0;
        bool trialInFlight = false;
        uint32 exhaustedDay = 0;
        bool authFailed = false;
        bool misconfigured = false;
        int32 markStatus = 0;
        std::vector<std::string> disabledSituations;
        BrainUsageRow today;
        bool dirty = false;
        BrainLatency latency;
        bool textRow = false;      // M6: the row of the text generator
        uint32 textCallsInFlight = 0;  // acquired text calls, counted against the daily request cap until Record
    };

    // Guarded by lock. A later UTC day moves every row to `finished` and starts clean ones; an earlier one (the
    // clock stepped back) changes nothing.
    void RolloverIfNeeded();
    EngineState* FindEngine(std::string_view name);
    EngineState const* FindEngine(std::string_view name) const;
    // Guarded by lock. None when the engine may take a call now, else why not. `probe` ignores the breaker and
    // the disabled situations. A half-open breaker counts as usable only while no trial call is in flight.
    BrainError Check(EngineState const& engine, std::string_view situation, bool probe, uint64 nowMs) const;
    // Guarded by lock. The MaxRequestsPerMinute bucket at nowMs, fractions kept (a refill every few ms adds less
    // than one token).
    void Refill(uint64 nowMs);
    double TokensAt(uint64 nowMs) const;

    mutable std::mutex lock;
    std::vector<EngineState> engines;  // chain order first, then engines that left the chain
    BrainUsageRow global;
    BrainUsageRow probes;
    bool globalDirty = false;
    bool probesDirty = false;
    std::vector<BrainUsageRow> finished;  // final rows of the previous day, not yet taken
    uint32 day = 0;
    uint64 reservedMicros = 0;
    uint32 probesInFlight = 0;
    double tokens = 0.0;
    uint64 lastRefillMs = 0;
    uint64 textReservedMicros = 0;  // M6: the part of reservedMicros held by text jobs
    // From Configure.
    uint64 capMicros = 1500000;
    std::array<uint32, 3> laneStopPercent{100, 80, 50};
    uint32 maxRequestsPerMinute = 120;
    uint32 breakerFailures = 5;
    uint32 breakerOpenMs = 30000;
    uint32 probeCap = 300;
    uint32 connectTimeoutMs = 2000;
    bool configuredOnce = false;  // the token bucket starts full at the first Configure
};

#endif
