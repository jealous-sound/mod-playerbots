/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINSTATS_H
#define PLAYERBOTS_BRAINSTATS_H

#include "BrainTypes.h"
#include <array>
#include <atomic>
#include <string>

// Why the voice director ran its heuristic although the brain was on.
enum class BrainFallback : uint8
{
    Submit = 0,  // Submit refused the job (BrainStats::submits says why)
    Timeout,     // no result before CoaVoice.DecisionTimeoutMs
    Error,       // the result carried an error (BrainStats::results says which)
    Invalid,     // a result without one valid answer
    AiOptOut,    // the speaker's account opted out of ai (.playerbots optout ai)
    WhisperCap,  // CoaVoice.WhisperBrainPerSenderPerDay reached for that speaker
    Busy,        // too many decisions waiting in the director
    Count
};

char const* BrainFallbackName(BrainFallback fallback);  // "submit", "timeout", "error", ...

// Process-wide counters, relaxed atomics: written by Submit (any thread), the workers and the director, read by
// .playerbots brain status and bot-status.json. They count since the process started.
struct BrainStats
{
    static BrainStats& Instance();

    std::array<std::atomic<uint64>, size_t(BrainSubmit::Count)> submits{};
    std::array<std::atomic<uint64>, size_t(BrainError::Count)> results{};
    std::atomic<uint64> inFlight{0};     // jobs a worker holds now
    std::atomic<uint64> sinkDropped{0};  // results no sink took (full)

    // The voice director (world thread).
    std::atomic<uint64> deferred{0};      // messages that waited for a decision
    std::atomic<uint64> byAnswer{0};      // resumed by a result
    std::atomic<uint64> byTimeout{0};     // resumed by the deadline
    std::atomic<uint64> brainReplied{0};  // the brain (calibrated) decided to answer
    std::atomic<uint64> brainSilent{0};   // the brain (calibrated) decided to stay silent
    std::atomic<uint64> ranked{0};        // an uncalibrated engine only ranked line, tone and emote
    std::atomic<uint64> stale{0};         // results for no waiting decision (late, other epoch)
    std::array<std::atomic<uint64>, size_t(BrainFallback::Count)> fallbacks{};

    void Count(BrainSubmit submit) { submits[size_t(submit)].fetch_add(1, std::memory_order_relaxed); }
    void Count(BrainError error) { results[size_t(error)].fetch_add(1, std::memory_order_relaxed); }
    void Count(BrainFallback fallback) { fallbacks[size_t(fallback)].fetch_add(1, std::memory_order_relaxed); }

private:
    BrainStats() = default;
    BrainStats(BrainStats const&) = delete;
    BrainStats& operator=(BrainStats const&) = delete;
};

namespace BrainStatus
{
// World thread (CoaStatusFile). Appends the value of bot-status.json's "brain" key: {"enabled":false} when the
// brain does not run, else the object described in the contract. Counters come from BrainStats' atomics, engine
// and budget rows from one short BrainGovernor lock.
void AppendJson(std::string& out);
}  // namespace BrainStatus

#endif
