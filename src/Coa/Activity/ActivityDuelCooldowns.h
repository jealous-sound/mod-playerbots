/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYDUELCOOLDOWNS_H
#define PLAYERBOTS_ACTIVITYDUELCOOLDOWNS_H

#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class BrainSink;
class Player;

// How a bot's own duel ended, for its "coa duel ended" emote.
enum class ActivityDuelOutcomeKind : uint8
{
    Won = 0,
    Lost,
    Fled,  // the bot fled (or forfeited by leaving the area)
};

struct ActivityDuelOutcome
{
    ActivityDuelOutcomeKind kind = ActivityDuelOutcomeKind::Won;
    ObjectGuid opponent;
    bool opponentIsBot = true;
    uint64 endMs = 0;  // CoaTime::NowMs()
};

// A duel that was fought (accepted, then won or fled), for the director (rings, info).
struct ActivityDuelResult
{
    ObjectGuid winner;
    ObjectGuid loser;
    bool winnerIsBot = true;
    bool loserIsBot = true;
    bool fled = false;
    uint64 endMs = 0;
};

// A duel accept waiting for the brain (CoaBrain.Use.Duel = 1, a real challenger).
enum class ActivityDuelVerdict : uint8
{
    Waiting = 0,  // no answer yet
    Yes,          // the brain said accept (p >= Duel.AcceptThreshold)
    No,           // the brain said decline
    Fallback,     // no usable answer (error, invalid) or the deadline passed: today's rule
};

struct ActivityDuelPending
{
    uint64 epoch = 0;          // the bot's CoaEpoch when it asked
    ObjectGuid challenger;
    ObjectGuid flagGuid;       // the duel arbiter from SMSG_DUEL_REQUESTED, for the accept/cancel packet
    uint64 jobId = 0;          // BrainTicket::jobId, to Cancel a job that timed out
    uint64 tag = 0;            // BrainJob::tag: matches the result to this entry
    uint64 deadlineMs = 0;     // CoaTime::NowMs() + CoaBrain.Duel.DeadlineMs
    ActivityDuelVerdict verdict = ActivityDuelVerdict::Waiting;
};

// The duel side store: challenge cooldowns, declines, the per-account daily count of duels fought against bots
// (Hero's Call Board farming cap), each bot's last outcome, the duels fought (for the director) and the duel accepts
// waiting for the brain. Map threads write it (challenges, accepts, the duel-end hook), the world thread drains it
// (the director), so it has its own mutex; every critical section is a few map operations, never a call out.
// With CoaActivity off nothing calls it: the seams test ActivityDirector::DuelRulesActive() first.
class ActivityDuelCooldowns
{
public:
    static ActivityDuelCooldowns& Instance();

    // --- Challenges: the bot's map thread ("coa duel partner", "coa challenge duel") ---

    // No pair cooldown between the two bots/players (either order).
    bool PairReady(uint32 lowA, uint32 lowB, uint64 nowMs) const;
    // A real player's account may be challenged: its PlayerCooldownMinutes passed and it did not decline a bot in
    // the last DECLINE_COOLDOWN_MS.
    bool PlayerReady(uint32 account, uint64 nowMs) const;
    // Duels fought between bots and this account on this UTC day (BrainClock::UtcDay, yyyymmdd).
    uint32 RealDuelsToday(uint32 account, uint32 utcDay) const;
    // When this bot's own last duel ended (0: none remembered).
    uint64 LastOwnDuelEndMs(uint32 botLow) const;
    // Checks and stamps in one critical section: the pair cooldown, and for a real target (account != 0) the player
    // cooldown. False (nothing stamped) when either still runs: another bot took the target first.
    bool TryStampChallenge(uint32 botLow, uint32 targetLow, uint32 targetAccount, uint64 nowMs,
                           uint32 pairCooldownMs, uint32 playerCooldownMs);

    // --- The duel-end hook: CoaPlayerScript::OnPlayerDuelEnd (map thread; the world thread for a cancel opcode) ---

    // Called before DuelComplete deletes the duel (both duel objects still exist). Reads only the two players.
    // - DUEL_INTERRUPTED before the start (StartTime 0), the loser is not the initiator: the loser declined the
    //   winner's challenge; a real decliner of a bot's challenge gets DECLINE_COOLDOWN_MS.
    // - A duel that started (StartTime set) and ended DUEL_WON or DUEL_FLED: each bot side gets its outcome, the
    //   result is queued for the director (at most 256 waiting; older ones dropped), and a bot-vs-real duel counts
    //   for the real side's account today.
    // `type` is the core's DuelCompleteType.
    void OnDuelEnd(Player* winner, Player* loser, uint8 type);

    // The bot's map thread: its own last outcome, once (removed). Outcomes older than 60 s are pruned unread.
    std::optional<ActivityDuelOutcome> TakeOutcome(uint32 botLow);

    // --- Duel accepts asked of the brain ---

    // Any thread. The sink every duel job posts to (created with the store; closed by Shutdown).
    std::shared_ptr<BrainSink> Sink() const { return sink; }
    // The bot's map thread (AcceptDuelAction), after a Queued submit: replaces any older entry of this bot.
    void BeginDecision(uint32 botLow, ActivityDuelPending const& pending);
    // The bot's map thread (ActivityDuelRules::Continue). nullopt: nothing waits for this bot in this epoch. Waiting
    // before its deadline: returned and kept. Yes, No, or past the deadline (returned as Fallback): removed.
    std::optional<ActivityDuelPending> TakeDecision(uint32 botLow, uint64 epoch, uint64 nowMs);
    // Monotonic tags for duel jobs (any thread).
    uint64 NextTag() { return nextTag.fetch_add(1, std::memory_order_relaxed) + 1; }

    // --- The director (world thread) ---

    // Drains the sink: each result finds its bot through pendingByTag (BrainResult::tag) and sets the entry's verdict:
    // Yes or No from ActivityBrain::ReadDuel(result, threshold), Fallback on an error or no valid answer. A result
    // whose entry is gone is dropped.
    void DrainBrain(float acceptThreshold);
    // Moves the queued results to `out`.
    void DrainResults(std::vector<ActivityDuelResult>& out);
    // Every 60 s: expired cooldowns, declines, other days' counts, outcomes older than 60 s, decisions 10 s past
    // their deadline.
    void Prune(uint64 nowMs, uint32 utcDay);
    // Director stop (CoaActivity turned off): forgets everything but the sink; waiting decisions vanish, so their bots
    // fall back to today's rule at their next Continue.
    void Clear();
    // OnShutdown: Clear, then closes the sink.
    void Shutdown();

    // Counters for .playerbots activity list (relaxed).
    uint64 Challenges() const { return challenges.load(std::memory_order_relaxed); }
    uint64 Declines() const { return declines.load(std::memory_order_relaxed); }
    uint64 RealDuels() const { return realDuels.load(std::memory_order_relaxed); }
    uint64 BrainAsked() const { return brainAsked.load(std::memory_order_relaxed); }

private:
    ActivityDuelCooldowns();
    ActivityDuelCooldowns(ActivityDuelCooldowns const&) = delete;
    ActivityDuelCooldowns& operator=(ActivityDuelCooldowns const&) = delete;

    static uint64 PairKey(uint32 lowA, uint32 lowB);

    struct DayCount
    {
        uint32 day = 0;
        uint32 count = 0;
    };

    mutable std::mutex lock;
    std::unordered_map<uint64, uint64> pairUntilMs;       // PairKey -> until
    std::unordered_map<uint32, uint64> playerUntilMs;     // account -> until (challenge cooldown)
    std::unordered_map<uint32, uint64> declinedUntilMs;   // account -> until
    std::unordered_map<uint32, DayCount> realDuelsByAccount;
    std::unordered_map<uint32, uint64> lastOwnDuelEndMs;  // bot low -> end
    std::unordered_map<uint32, ActivityDuelOutcome> outcomes;       // bot low -> its last outcome
    std::vector<ActivityDuelResult> results;                        // for the director, at most 256
    std::unordered_map<uint32, ActivityDuelPending> pending;        // bot low -> decision
    std::unordered_map<uint64, uint32> pendingByTag;                // BrainJob::tag -> bot low
    std::shared_ptr<BrainSink> sink;                                // never null
    std::atomic<uint64> nextTag{0};
    std::atomic<uint64> challenges{0};
    std::atomic<uint64> declines{0};
    std::atomic<uint64> realDuels{0};
    std::atomic<uint64> brainAsked{0};
};

#endif
