/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOCKTEXTENGINE_H
#define PLAYERBOTS_MOCKTEXTENGINE_H

#include "ITextEngine.h"
#include <atomic>
#include <string>
#include <vector>

// What a mock call does instead of answering.
enum class MockTextFault : uint8
{
    None = 0,   // answer with the next scripted or built-in line
    Refusal,    // error Refused, finishReason "refusal"
    Timeout,    // wait for the budget (in 20 ms slices, honouring cancel), then error Timeout
    Http402,    // error Exhausted, HTTP 402
    Http429,    // error Exhausted, HTTP 429
    Http500,    // error ServerError, HTTP 500
    Garbage,    // error BadResponse: a body that is not JSON
    Empty,      // error Refused: a 200 with no text (cut off)
    Injection,  // answer with a hostile line: commands, links, pipes, AI talk (the finisher must reject it)
    Long,       // answer with 600 bytes of French words (the finisher must cut it)
    Mixed       // a cycle: None, Refusal, None, Timeout, None, Http500, None, Garbage, None, Empty, None, Injection,
                // None, Long (never 402 or 429: they would switch the engine off for the day)
};

MockTextFault MockTextFaultFromName(std::string const& name);  // "none", "refusal", "timeout", "402", "429", "500",
                                                               // "garbage", "empty", "injection", "long", "mixed"
char const* MockTextFaultName(MockTextFault fault);

// Text.Provider = mock: no socket. Each call waits Text.Mock.LatencyMs (in 20 ms slices, honouring the cancel flag
// and the budget: past the budget -> Timeout after budget ms), then, for FaultPercent of the calls (a
// deterministic counter, not a random draw: call n faults when (n * FaultPercent) mod 100 < FaultPercent), does
// what Text.Mock.Fault says; other calls answer with the next of eight built-in French lines ("Salut {name} !",
// ...) or of the script. usage: input tokens = prompt bytes / 4, output tokens = text bytes / 4, cost =
// Text.Mock.UsdPerCall (else tokens x the configured prices). Thread-safe: its only state is an atomic counter.
class MockTextEngine final : public ITextEngine
{
public:
    // A scripted call: call i (cycling) does steps[i % size].
    struct Step
    {
        MockTextFault fault = MockTextFault::None;
        std::string text;          // None: the line to answer with ("" = the next built-in one)
        uint32 latencyMs = 0;      // 0: Text.Mock.LatencyMs
        uint64 usdMicros = 0;      // 0: Text.Mock.UsdPerCall
    };

    explicit MockTextEngine(BrainTextConfig config);
    // Tests: a script replaces Text.Mock.Fault. Given at construction: the engine is immutable afterwards.
    MockTextEngine(BrainTextConfig config, std::vector<Step> script);

    BrainTextConfig const& Config() const override { return config; }
    TextReply Generate(TextCall const& call, BrainHttpClient& http, uint32 budgetMs) override;

    uint64 Calls() const { return calls.load(std::memory_order_relaxed); }

private:
    BrainTextConfig const config;
    MockTextFault const fault;
    std::vector<Step> const script;
    std::atomic<uint64> calls{0};
};

#endif
