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

// M6: why the voice lane said a template line although it had asked the text generator for one.
enum class BrainTextFallback : uint8
{
    Submit = 0,  // Submit refused the job (BrainStats::submits says why: budget, throttle, full...)
    Timeout,     // no result before CoaVoice.TextTimeoutMs
    Error,       // the result carried an error other than Refused and Rejected (network, server, breaker...)
    Refused,     // the model refused, or left no text (BrainError::Refused)
    Invalid,     // the sanitizer or the validator rejected the text (BrainError::Rejected, or at emit time)
    Repeat,      // the repetition filter matched the generated line at emit time
    Filtered,    // sObjectMgr->IsChatFiltered or CoaVoice.BlockedWords matched it at emit time
    Busy,        // too many text jobs waiting in the director
    SenderCap,   // CoaVoice.TextPerSenderPerDay reached for that speaker
    OptOut,      // the speaker's account opted out of ai (.playerbots optout ai)
    Count
};

char const* BrainTextFallbackName(BrainTextFallback fallback);  // "submit", "timeout", "error", "refused", "invalid",
                                                                // "repeat", "filtered", "busy", "sender-cap", "opt-out"

// M6: the fallback a failed text result stands for. The director counts it and the fault test checks that every
// failure has one: Refused -> Refused; Rejected -> Invalid; Timeout and TooLate -> Timeout; every other error
// (Cancelled, NoEngine, Dns, Network, ServerError, BadRequest, AuthFailed, Misconfigured, Exhausted, BadResponse,
// NotBuilt) -> Error. BrainError::None has no fallback: it returns Error (callers test the error first).
BrainTextFallback BrainTextFallbackOf(BrainError error);

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

    // M6, the text lane (voice director, world thread).
    std::atomic<uint64> textChosen{0};     // decisions that picked free_text
    std::atomic<uint64> textAsked{0};      // text jobs submitted for a voice line (Queued)
    std::atomic<uint64> textGenerated{0};  // generated lines that passed every check and replaced the template
    std::atomic<uint64> textSent{0};       // generated lines said
    std::atomic<uint64> textStale{0};      // text results with no waiting line (other epoch, line dropped)
    std::array<std::atomic<uint64>, size_t(BrainTextFallback::Count)> textFallbacks{};

    // M6, the banter (CoaVoiceBanter, world thread).
    std::atomic<uint64> banterExchanges{0};  // exchanges started: a zone, a bot and a topic chosen
    std::atomic<uint64> banterOpeners{0};    // openers said
    std::atomic<uint64> banterText{0};       // of them written by the text model
    std::atomic<uint64> banterAnswers{0};    // answers of a second bot (BOT events of banter openers) said
    std::atomic<uint64> banterSkipped{0};    // exchanges given up: the bot left, the line was rejected...
    std::atomic<uint64> banterEmotes{0};     // openers with an emote chosen by the brain (CoaBrain.Use.Emote)

    // M6, the RPG advice (BrainRpgAdvice).
    std::atomic<uint64> rpgAsked{0};     // advice jobs submitted (Queued)
    std::atomic<uint64> rpgAnswered{0};  // advice entries published
    std::atomic<uint64> rpgApplied{0};   // NewRpg status picks that used an entry (Blend returned true)
    std::atomic<uint64> rpgEntries{0};   // gauge: entries of the current snapshot

    void Count(BrainSubmit submit) { submits[size_t(submit)].fetch_add(1, std::memory_order_relaxed); }
    void Count(BrainError error) { results[size_t(error)].fetch_add(1, std::memory_order_relaxed); }
    void Count(BrainFallback fallback) { fallbacks[size_t(fallback)].fetch_add(1, std::memory_order_relaxed); }
    void Count(BrainTextFallback fallback) { textFallbacks[size_t(fallback)].fetch_add(1, std::memory_order_relaxed); }

private:
    BrainStats() = default;
    BrainStats(BrainStats const&) = delete;
    BrainStats& operator=(BrainStats const&) = delete;
};

namespace BrainStatus
{
// World thread (CoaStatusFile). Appends the value of bot-status.json's "brain" key: {"enabled":false} when the
// brain does not run, else the object described in the contract. Counters come from BrainStats' atomics, engine
// and budget rows from one short BrainGovernor lock. M6: the object gains a "text" block (contract, section 14):
// provider, usability, queue depths, the text row's day, the voice lane's, the banter's and the RPG advice's counters.
void AppendJson(std::string& out);
}  // namespace BrainStatus

#endif
