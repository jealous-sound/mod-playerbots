/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINTYPES_H
#define PLAYERBOTS_BRAINTYPES_H

#include "Define.h"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Plain values shared by every brain layer. Nothing here points at a game object: a job carries raw GUIDs, an
// epoch and strings, so the worker threads that carry it never touch the world.

// Queue lanes, served in this order. CoaBrain.LaneStopAt stops the lower lanes first as the day's budget goes.
enum class BrainLane : uint8
{
    Player = 0,  // a real player waits: voice replies, .playerbots brain test
    Organizer,   // world-level decisions (M5) and .playerbots brain probe
    Ambient,     // background decisions (M4+): the first to stop, the only one that evicts
    Count
};

// What a job is for. ChatReply is gated by CoaBrain.Use.ChatReply; Probe and Test are GM tools.
enum class BrainUse : uint8
{
    ChatReply = 0,  // the voice director's decision for one human message
    Probe,          // one engine (BrainJob::forceEngine), no cache, no chain
    Test,           // the chain, no cache, nothing is said in game
    Count
};

// Submit's verdict. Anything but Queued or Answered means: the caller runs today's code now.
enum class BrainSubmit : uint8
{
    Queued = 0,  // a worker will post exactly one BrainResult to the sink (unless the job is cancelled)
    Answered,    // served from the cache: *inlineHit is filled, nothing is posted
    Off,         // not running, shutting down, or this use is off
    Invalid,     // no question, a question without its options, an unknown forceEngine
    NoEngine,    // every engine of the chain is open, exhausted, misconfigured or past its own daily caps
    OverBudget,  // the global daily budget, this lane's watermark of it, or Probe.MaxPerDay is spent
    Throttled,   // MaxRequestsPerMinute
    TooLate,     // the time left before the deadline is under the first usable engine's p95
    Full,        // the lane is full
    Count
};

enum class BrainError : uint8
{
    None = 0,       // answered; single answers may still be invalid (BrainAnswer::valid)
    TooLate,        // the deadline passed before a worker took the job or before any engine could be tried
    Cancelled,      // Cancel(), eviction from the Ambient lane, or shutdown
    NoEngine,       // on the worker no engine could take it (breakers, caps, time left under its p50)
    Dns,            // no cached address for the host yet (the resolver is asked again)
    Network,        // connect refused or reset, TLS handshake or certificate failure
    Timeout,        // connect or read went past the call's budget
    ServerError,    // HTTP 408, 5xx, 524, 529: counts toward the breaker
    BadRequest,     // HTTP 400, 413, 422: not retried; the situation is disabled for that engine until reset
    AuthFailed,     // HTTP 401, 403: the engine is off until .reload config or .playerbots brain reset
    Misconfigured,  // HTTP 404 (wrong URL), no key, bad URL: off until .reload config or reset
    Exhausted,      // HTTP 402, 429 or a spend-limit error text: off until the next UTC day or reset
    BadResponse,    // a 200 whose body is not the dialect's shape, or larger than 64 KiB
    NotBuilt,       // an engine kind that is not built yet (openai-constrained before M2b)
    Count
};

enum class BrainQuestionType : uint8
{
    Choice = 0,  // one option key among the listed ones (Jev "choice")
    YesNo,       // the probability that a statement is true (Jev "noul")
    Score,       // a position on an ordered scale of 2 to 10 levels (Jev "score")
};

struct BrainOption
{
    std::string key;    // Choice: the id sent and returned; Score: "0".."n-1"; YesNo: "true", "false"
    std::string text;   // its description (the criteria value): English wording, or a template row
    std::string group;  // Choice, optional: options of one group are cached as one (the line's reply name)
};

struct BrainQuestion
{
    std::string name;  // key in "questions" and in "answers": reply, who, line, tone, emote
    BrainQuestionType type = BrainQuestionType::Choice;
    std::string instructions;
    // Choice: 1 to 255 options. Score: 2 to 10 levels, lowest first. YesNo: none, or exactly "true" and "false".
    std::vector<BrainOption> options;
    bool cacheable = true;  // false: never cached (who names other bots every time)
};

struct BrainAnswer
{
    std::string name;
    BrainQuestionType type = BrainQuestionType::Choice;
    bool valid = false;        // false: only this question falls back to the heuristic
    bool calibrated = false;   // p came from the provider (probabilities, noul), not from UncalibratedConfidence
    std::string choice;        // Choice: an option key; a group when the result came from the cache
    float p = 0.0f;            // Choice: p of `choice`; YesNo: p of true; Score: p of `level`
    float confidence = -1.0f;  // the provider's confidence when it sent one
    float score = -1.0f;       // Score: fractional position in [0, levels - 1]
    int32 level = -1;          // Score: argmax of the probabilities, else round(score)
    std::vector<std::pair<std::string, float>> dist;  // Choice: key -> p; Score: "0".."n-1" -> p; best first
};

struct BrainUsage
{
    uint32 inputTokens = 0;
    uint32 outputTokens = 0;
    uint64 usdMicros = 0;       // usage.cost when reported, else tokens x price, else the byte estimate
    bool costReported = false;  // usage.cost was in the response
};

struct BrainJob
{
    uint64 id = 0;    // set by Submit
    uint64 tag = 0;   // the caller's own id (the director's event id), echoed in the result
    uint32 epoch = 0; // the caller's epoch, echoed: a result of another epoch is stale to the caller
    BrainUse use = BrainUse::ChatReply;
    BrainLane lane = BrainLane::Player;
    uint64 botGuid = 0;     // raw ObjectGuid of the bot concerned (logs, decision ring), 0 for none
    std::string situation;  // "chat_reply", "probe", "test": per-engine disable on HTTP 400/413/422, logs
    std::string state;      // features-only state text, at most CoaBrain.StateMaxChars bytes
    std::string playerText; // the player's words, sanitized; sent only to engines allowed to receive them
    std::vector<BrainQuestion> questions;
    std::string cacheKey;     // empty: never cached; the brain appends the chain's engines and models
    std::string forceEngine;  // Probe: only this engine, whatever its breaker says
    bool record = false;      // Probe: the result keeps the raw request and response bodies
    uint64 submitMs = 0;      // CoaTime::NowMs(), set by Submit
    uint64 deadlineMs = 0;    // CoaTime::NowMs() clock; after it the result is useless to the caller
};

struct BrainResult
{
    uint64 jobId = 0;
    uint64 tag = 0;
    uint32 epoch = 0;
    BrainUse use = BrainUse::ChatReply;
    BrainError error = BrainError::None;
    std::string engine;         // the engine that answered, or the last one tried
    std::string providerModel;  // the response's "model"
    bool calibrated = false;    // the answering engine is Engine.<n>.Calibrated
    bool fromCache = false;
    std::vector<BrainAnswer> answers;  // one per job question, same order; empty on error
    BrainUsage usage;
    int32 httpStatus = 0;
    uint32 queuedMs = 0;       // Submit to the worker taking it
    uint32 callMs = 0;         // the worker's time on the job, every engine tried
    uint32 enginesTried = 0;
    std::string detail;        // error excerpt, at most 300 bytes, never a key
    std::string url;           // record only: where the request went
    std::string rawRequest;    // record only
    std::string rawResponse;   // record only

    // The answer to the question of that name, null when absent.
    BrainAnswer const* Find(std::string_view name) const;
};

char const* BrainLaneName(BrainLane lane);                  // "player", "organizer", "ambient"
char const* BrainUseName(BrainUse use);                     // "chat-reply", "probe", "test"
char const* BrainSubmitName(BrainSubmit submit);            // "queued", "answered", "off", "invalid", ...
char const* BrainErrorName(BrainError error);               // "none", "too-late", "cancelled", ...
char const* BrainQuestionTypeName(BrainQuestionType type);  // "choice", "yes-no", "score"

// UTC calendar days: the budgets reset at UTC midnight.
namespace BrainClock
{
uint32 UtcDay(int64 unixSeconds);  // yyyymmdd, proleptic Gregorian, integer arithmetic only
uint32 UtcDayNow();                // UtcDay(time(nullptr))
uint32 SecondsToUtcMidnight(int64 unixSeconds);
}  // namespace BrainClock

#endif
