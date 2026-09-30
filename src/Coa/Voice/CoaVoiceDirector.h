/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICEDIRECTOR_H
#define PLAYERBOTS_COAVOICEDIRECTOR_H

#include "BrainSink.h"
#include "BrainStateBuilder.h"
#include "BrainTypes.h"
#include "CoaEventBus.h"
#include "CoaVoiceCandidateIndex.h"
#include "CoaVoiceConfig.h"
#include "CoaVoiceEvent.h"
#include "CoaVoiceGovernor.h"
#include "Define.h"
#include <array>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

namespace NameMatch
{
class Prepared;
}

// The one place that decides whether bots answer real players, which bot, and what it says. World thread only,
// in CoaWorldScript::OnUpdate after the map threads are joined, so it may touch any Player; it keeps nothing but
// GUIDs between ticks. Pipeline per event: resolve the speaker and scope -> nominate (whisper receiver, group
// members, guild or zone bucket of the candidate index) -> score -> heuristic reply roll (a named bot always
// answers; past the scope's chance only named bots may) -> shortlist through the governor, named bots first ->
// intent and template line -> schedule 4-12 s later (10-25 s in combat) -> send through the scope's route.
// With CoaVoice.Enable = 0 Update returns at once.
class CoaVoiceDirector
{
public:
    static CoaVoiceDirector& Instance();

    // World thread, every CoaWorldScript::OnUpdate. Handles the drained bus events (the vector is consumed:
    // cleared on return), refreshes the candidate index when stale, sends the lines that are due.
    void Update(uint32 diff, std::vector<CoaEvent>& events);

    // World thread, CoaWorldScript::OnShutdown. Drops every waiting line; nothing is sent after the last tick.
    void Shutdown();

    // World thread (.playerbots voice stats). Several lines separated by '\n'.
    std::string FormatStats() const;

private:
    CoaVoiceDirector() = default;
    CoaVoiceDirector(CoaVoiceDirector const&) = delete;
    CoaVoiceDirector& operator=(CoaVoiceDirector const&) = delete;

    struct LaterFirst
    {
        bool operator()(CoaVoiceLine const& a, CoaVoiceLine const& b) const { return a.dueMs > b.dueMs; }
    };

    struct Stats
    {
        std::array<uint64, size_t(CoaVoiceScopeKind::Count)> events{};     // events handled, per scope
        std::array<uint64, size_t(CoaVoiceScopeKind::Count)> scheduled{};  // answers decided
        std::array<uint64, size_t(CoaVoiceScopeKind::Count)> sent{};       // lines said
        std::array<uint64, size_t(CoaVoiceDrop::Count)> drops{};
        uint64 botEvents = 0;
        uint64 denies = 0;       // refusals sent
        uint64 denyRepeats = 0;  // refusals held back by the repeat delay
    };

    // Per-event cost on the world thread, in microseconds: the last 1024 samples of each scope.
    struct CostRing
    {
        std::array<uint32, 1024> samples{};
        uint32 next = 0;
        uint32 count = 0;
        void Add(uint32 us);
        void Percentiles(uint32& p50, uint32& p95) const;
    };

    void Reset();
    // True once the scope was resolved (the event's cost is then recorded).
    bool HandleEvent(CoaVoiceConfig const& config, CoaEvent& event, uint64 nowMs);
    // Fills `scope` from the event and the live speaker; false (with the drop counted) when it cannot be answered.
    bool ResolveScope(CoaEvent const& event, Player* sender, CoaVoiceScope& scope);
    // Fills `out` with the scored candidates; CoaVoiceDrop::Count when there is one, else why there is none.
    CoaVoiceDrop Nominate(CoaVoiceConfig const& config, CoaEvent const& event, CoaVoiceScope const& scope,
                          Player* sender, NameMatch::Prepared const& words, std::vector<CoaVoiceCandidate>& out);
    bool Eligible(CoaVoiceScope const& scope, Player* bot, Player* sender, bool fromBot) const;
    int32 Score(CoaVoiceScope const& scope, Player* bot, Player* sender, bool mentioned) const;
    // Percent chance (0 to 100) that the event is answered; `named`: a bot's forced chance when it is named.
    double ReplyChance(CoaVoiceConfig const& config, CoaVoiceScopeKind kind, bool named, uint8 depth) const;
    // Picks and fills a line; false when every candidate is missing, rejected or a repeat (`why`: NoLine or
    // Repetition). `named`: the bot's name is in the text.
    bool Compose(CoaVoiceConfig const& config, CoaVoiceScope const& scope, Player* bot, Player* sender,
                 CoaIntent intent, bool named, uint32 textEmote, uint64 nowMs, CoaVoiceLine& line, CoaVoiceDrop& why);
    void Schedule(CoaVoiceConfig const& config, CoaVoiceLine&& line);
    // A stranger's whisper the voice leaves unanswered: the localized refusal, as the legacy code would have sent.
    void ScheduleDeny(CoaVoiceConfig const& config, CoaVoiceScope const& scope, Player* bot, Player* sender,
                      uint64 nowMs);
    void SendDue(CoaVoiceConfig const& config, uint64 nowMs);
    bool Send(CoaVoiceConfig const& config, CoaVoiceLine const& line);
    // CoaVoice.BotToBot: a bot line said in party, raid, guild or a channel (no hook sees those) becomes a BOT
    // event handled next tick. Say and yell get theirs from the hook.
    void QueueBotEcho(CoaVoiceLine const& line, Player* bot);
    void CountDrop(CoaVoiceDrop drop) { ++stats.drops[size_t(drop)]; }

    // M2: a human message waiting for its brain decision (CoaBrain.Enable = 1), keyed by event id.
    struct PendingDecision
    {
        uint64 jobId = 0;
        uint32 epoch = 0;        // brainEpoch at submit
        uint64 eventId = 0;
        uint64 eventMs = 0;      // tickNowMs of the event: the reply delay counts from here
        uint64 deadlineMs = 0;   // eventMs + CoaVoice.DecisionTimeoutMs
        uint64 submitMs = 0;
        CoaEvent event;          // copy: sender, target, text, chain depth
        CoaVoiceScope scope;
        std::vector<CoaVoiceCandidate> nominees;  // ballot order, cut to the shortlist, only bots found at submit
        CoaIntent intent = CoaIntent::None;
        bool anyHit = false;     // the heuristic draw made at event time
        bool namedOnly = false;
        bool forced = false;     // a nominee is named in party, raid, guild, say, yell or a channel (M1 answers)
        BrainChatPlan plan;
    };

    // The brain's answers as the ballot uses them.
    struct BrainPick
    {
        bool fromBrain = false;    // a result with at least one valid answer
        bool calibrated = false;   // result.calibrated
        bool replyKnown = false;   // a calibrated reply answer, or a calibrated line answer topped by silent
        bool silent = false;       // with replyKnown: nobody answers
        float replyP = -1.0f;
        int32 whoIndex = -1;       // index into PendingDecision::nominees (calibrated only)
        std::vector<uint32> lineOrder;  // indexes into plan.lines, best first, silent removed
        std::string lineKey;       // the top line key (ring)
        float lineP = -1.0f;
        int32 tone = -1;           // 0..4
        int32 emote = -1;          // TextEmotes id, 0 = none, -1 = not answered (persona roll)
        std::string emoteKey;
        bool fromCache = false;
    };

    // Returns true when the event is now the brain's (waiting, or answered from the cache and resumed).
    bool TrySubmitDecision(CoaVoiceConfig const& config, CoaEvent const& event, CoaVoiceScope const& scope,
                           Player* sender, CoaIntent intent, bool anyHit, bool namedOnly, bool forced, uint64 nowMs);
    // Results first (stale ones counted and dropped), then the decisions past their deadline (Cancel, timeout).
    void DrainDecisions(CoaVoiceConfig const& config, uint64 nowMs);
    // result null: timed out.
    void ResumeDecision(CoaVoiceConfig const& config, PendingDecision& decision, BrainResult const* result,
                        uint64 nowMs);
    BrainPick ReadDecision(CoaVoiceConfig const& config, PendingDecision const& decision,
                           BrainResult const& result) const;
    // The M1 ballot on sorted candidates: shortlist gates, compose, schedule. pick and plan null: exactly M1.
    // Returns the responders; `why` gets the reason when there is none.
    uint32 RunBallot(CoaVoiceConfig const& config, CoaEvent const& event, CoaVoiceScope const& scope, Player* sender,
                     std::vector<CoaVoiceCandidate>& candidates, CoaIntent intent, bool namedOnly, uint64 eventMs,
                     uint64 nowMs, BrainPick const* pick, BrainChatPlan const* plan, CoaVoiceDrop& why);
    // The gate RunBallot would drop the event on: the first one of the shortlisted bots it tries (only the
    // mentioned ones under namedOnly), when every one of them is held back; CoaVoiceDrop::Count when one may answer.
    CoaVoiceDrop BallotGate(CoaVoiceConfig const& config, std::vector<CoaVoiceCandidate> const& candidates,
                            CoaVoiceScope const& scope, ObjectGuid sender, bool namedOnly, uint64 nowMs) const;
    // A brain-ranked line for `bot`, or false (then M1 Compose runs).
    bool ComposeFromBrain(CoaVoiceConfig const& config, CoaVoiceScope const& scope, Player* bot, Player* sender,
                          CoaIntent intent, BrainPick const& pick, BrainChatPlan const& plan, uint64 nowMs,
                          CoaVoiceLine& line);
    // M1's `drop` + `fail` lambdas as a member: counts, logs, and sends the whisper refusal when due.
    void FailEvent(CoaVoiceConfig const& config, CoaEvent const& event, CoaVoiceScope const& scope, CoaVoiceDrop why,
                   uint64 nowMs);

    std::shared_ptr<CoaVoiceConfig const> config;  // snapshot of the current tick
    bool active = false;                           // state exists and must be dropped if voice turns off
    uint32 locale = 0;                             // PlayerbotTextMgr::GetLocalePriority() of the current tick
    uint64 nextEventId = 1;
    uint64 nextPruneMs = 0;
    CoaVoiceCandidateIndex index;
    CoaVoiceGovernor governor;
    std::priority_queue<CoaVoiceLine, std::vector<CoaVoiceLine>, LaterFirst> pending;
    std::vector<CoaEvent> botEchoes;                // BOT events made by QueueBotEcho, handled next tick
    std::unordered_map<uint64, uint64> denyRepeat;  // CoaHash(bot, sender, deny name) -> last sent ms
    Stats stats;
    std::array<CostRing, size_t(CoaVoiceScopeKind::Count)> costs;
    uint64 tickNowMs = 0;                           // CoaTime::NowMs() of the current tick
    uint64 currentEventId = 0;                      // the event HandleEvent is on (logs, refusal lines)
    bool closed = false;                            // after Shutdown: nothing is handled or sent any more
    std::vector<CoaVoiceCandidate> nominees;        // reused by HandleEvent
    std::unordered_set<std::string> botFirstNames;  // folded first names of the indexed bots (vocative test)
    // Folded first names of the online players -> how many players have each (vocative test).
    std::unordered_map<std::string, uint32> humanFirstNames;

    std::unordered_map<uint64, PendingDecision> decisions;  // event id -> decision
    std::shared_ptr<BrainSink> brainSink;                   // created at the first submit, closed at Shutdown
    std::vector<BrainResult> brainResults;                  // reused by DrainDecisions
    uint32 brainEpoch = 1;                                  // ++ at Reset and Shutdown
    std::unordered_map<uint32, uint32> whisperBrainToday;   // speaker account -> whisper jobs today
    uint32 whisperBrainDay = 0;                             // UTC yyyymmdd of that map
    ObjectGuid lastResponder;                               // set by RunBallot: its first scheduled bot (ring)
};

#endif
