/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICEDIRECTOR_H
#define PLAYERBOTS_COAVOICEDIRECTOR_H

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
};

#endif
