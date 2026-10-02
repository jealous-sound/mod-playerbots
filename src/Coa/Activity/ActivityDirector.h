/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYDIRECTOR_H
#define PLAYERBOTS_ACTIVITYDIRECTOR_H

#include "ActivityBrain.h"
#include "ActivityCandidates.h"
#include "ActivityCensus.h"
#include "ActivityKind.h"
#include "ActivityOperations.h"
#include "ActivityOrganizer.h"
#include "ActivityRecruitment.h"
#include "ActivityStore.h"
#include "ActivityTypes.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class BrainSink;
class Player;
struct ActivityConfig;
struct ActivityRegistrySnapshot;

// The activity director (M4): schedules, plans, runs and ends every activity, and is the only writer of the
// registry. World thread only, ticked from CoaWorldScript::OnUpdate after memory and voice (the map threads are
// joined): it may read and change any Player there. Every public function but the static gates is world thread only
// (COA_ASSERT_WORLD_THREAD). With AiPlayerbot.CoaActivity.Enable = 0 nothing here runs but the ruleset restores an
// earlier run left: CoaWorldScript never calls Update while ActivityConfig::Enabled(), Running() and Restoring() are
// false, and every seam tests a gate below first.
class ActivityDirector
{
public:
    static ActivityDirector& Instance();

    // --- Lock-free gates for the seams (any thread, one relaxed load each; set by the director) ---

    // Enable = 1 and the tables were found: the director runs.
    static bool Running() { return running.load(std::memory_order_relaxed); }
    // Running and Duel.Enable: AcceptDuelAction's layered rule, the duel-end hook, the "coa duel" strategy.
    static bool DuelRulesActive() { return duelRules.load(std::memory_order_relaxed); }
    // DuelRulesActive and Duel.Ambient: AiFactory gives "coa duel" to non-participant random bots.
    static bool DuelAmbient() { return duelAmbient.load(std::memory_order_relaxed); }
    // War Mode an activity gave is still to be taken back from a bot that was offline or loading (pendingRestore not
    // empty): CoaWorldScript keeps calling Update, which restores it at the bot's next login, even with activities off.
    static bool Restoring() { return restoring.load(std::memory_order_relaxed); }

    // --- CoaWorldScript ---

    // OnStartup, after the preferences. Enable = 1: CheckTables (sync; missing: one LOG_ERROR, stays off), the crash
    // recovery (AbortLiveRows, LoadUnrestored), LoadMaxId, then running. Enable = 0: CheckTables and LoadUnrestored
    // only, silently (the restores a crash or a stop left; nothing else runs).
    void OnStartup();
    // OnAfterConfigLoad(reload = true), after ActivityConfig::Publish. Off -> on: as OnStartup. On -> off: Stop now
    // (every activity ended with outcome Disabled, groups disbanded, rulesets restored, registry emptied).
    // On -> on: the gates follow the new Duel.Enable and Duel.Ambient; running activities keep going.
    void OnConfigReload();
    // Every OnUpdate while ActivityConfig::Enabled(), Running() or Restoring(). Drains the duel brain sink on every
    // call; the rest runs every TickMs (contract section 5.3). Activities off: only the pending restores, every second.
    void Update(uint32 diff);
    // OnShutdown, after memory: every activity ended (Shutdown) with its groups disbanded and rulesets restored on
    // this thread, pending restores tried once more (the rest waits in the store for the next start); the sinks
    // closed; nothing runs afterwards.
    void Shutdown();

    // --- .playerbots activity (world thread; lines to print, English for GMs) ---

    std::vector<std::string> List() const;
    std::vector<std::string> Info(uint32 id) const;
    // GM diagnostic: current RareScout member/registry and loaded target only. Synthetic hook arguments;
    // no spell preparation, effects, packets, registry changes or gameplay mutations.
    std::vector<std::string> VerifyM7(uint32 id);
    // Plans and starts at once (no roll; caps and the load guard kept). The join decisions are the dice (no brain
    // wait). `gmPlayer` places it when the request names no spot (may be null from the console).
    std::vector<std::string> Start(ActivityStartRequest request, Player* gmPlayer);
    // id 0: every activity. Outcome Stopped.
    std::vector<std::string> Stop(uint32 id);
    std::string Pause();
    std::string Resume();
    std::vector<std::string> Census(uint32 hours) const;
    uint32 CleanRuns(ActivityKindId kind) const;
    void RecordCleanWave();
    // SEC_PLAYER. id 0: the nearest joinable activity (group kinds first: same team, band +-5, same map). Returns the
    // player's line (coa_act_join_*, localized; coa_act_off when not running).
    std::string Join(Player* player, uint32 id);
    // M7 SEC_PLAYER: explicit escort request, ordinary invite only; requester GUID owns cancellation.
    // Rejected requests are throttled for 5 s; the player cooldown starts only after a live activity is created.
    std::vector<std::string> RequestEscort(Player* player, std::string target);
    std::vector<std::string> RequestOwned(Player* player, ActivityKindId kind, std::string target);
    std::vector<std::string> CancelOwned(Player* player, ActivityKindId kind);
    std::vector<std::string> ServiceOffers(Player* player, uint32 recipeSpell);
    std::string AcceptGoldRoll(Player* player, uint32 activityId);
    std::string ReserveRaid(Player* player, uint32 activityId, uint32 itemId);
    std::vector<std::string> CancelEscort(Player* player);
    // Configured names on the player's map/team only, bounded to 16 names and a 384-byte localized line.
    std::vector<std::string> EscortDestinations(Player* player) const;

private:
    ActivityDirector();
    ActivityDirector(ActivityDirector const&) = delete;
    ActivityDirector& operator=(ActivityDirector const&) = delete;

    // A plan waiting for its join decisions (Planning).
    struct PendingPlan
    {
        uint64 planId = 0;
        ActivityPlan plan;
        uint64 jobId = 0;         // Organizer job, 0 when the dice decide
        uint64 deadlineMs = 0;    // BrainDeadlineMs after the submit
        bool answered = false;
        std::vector<uint32> asked;  // candidate index of each join question (b<i> -> candidates[asked[i]])
    };

    // A "which activity next" question waiting for the brain.
    struct PendingSchedule
    {
        uint64 tag = 0;
        uint64 jobId = 0;
        uint64 deadlineMs = 0;
        std::vector<ActivityKindId> eligible;
        std::vector<ActivityBrain::NextOption> options;  // M5: what the "next" question offered (kinds, targets)
        ObjectGuid nearGuid;      // the real player it is for, looked up again when the answer comes
        bool answered = false;
        ActivityBrain::NextPick pick;
        bool active = false;
    };

    void StartRunning(bool fromReload);
    void StopRunning(ActivityOutcome outcome);
    void Tick(uint64 nowMs, uint32 nowUnix);
    void CollectRealPlayers();
    void LoadGuard(uint64 nowMs);
    void MemberPass(Activity& activity, uint64 nowMs, uint32 nowUnix);
    void HandleLeaves(Activity& activity, uint32 nowUnix);
    void EndActivity(Activity& activity, ActivityOutcome outcome, uint32 nowUnix);
    void Schedule(uint64 nowMs, uint32 nowUnix);
    void ResolveSchedule(uint64 nowMs, uint32 nowUnix);
    void DrainOrganizer();
    void RestorePending();
    void LoadPendingRestores();
    void SyncRestoring();  // restoring = !pendingRestore.empty()
    void RouteDuelResults(uint64 nowMs, uint32 nowUnix);
    bool BeginPlan(ActivityStartRequest const& request, bool askBrain, std::string& why);
    void ResolvePlans(uint64 nowMs, uint32 nowUnix, bool force);
    void Publish();
    uint32 Capacity() const;
    ActivityKind* KindOf(ActivityKindId kind) const;
    ActivityServices Services(uint64 nowMs, uint32 nowUnix);
    // Private helpers (director implementation).
    void RebuildHeld();
    void Travel(Activity& activity, ActivityMember& member, Player* bot, Player* leader, uint64 nowMs);
    void GroupPass(Activity& activity, Player* leader, uint64 nowMs);
    void Invitations(Activity& activity, Player* leader, uint64 nowMs, uint32 nowUnix);
    void WarMode(Activity& activity, uint32 nowUnix);
    void RestoreRuleset(ActivityMember& member);
    void SyncStore(Activity& activity);
    bool DicePlan(std::vector<ActivityKindId> const& eligible, Player* nearPlayer);
    uint32 ResolvePlan(PendingPlan& pending, uint64 nowMs, uint32 nowUnix);
    std::vector<ActivityKindId> EligibleKinds() const;
    uint32 LiveCount(ActivityKindId kind) const;
    bool RareTargetBusy(std::string const& target, uint8 team, ActivityKindId kind = ActivityKindId::RareScout) const;
    void PruneEscortRequests(uint64 nowMs);
    uint32 MinBots(ActivityKindId kind) const;
    uint32 Participants() const;
    uint32 ParticipantCap() const;
    ActivityBrain::Scene SceneNow() const;
    Player* PickNear() const;
    void MarkStrategiesDirty(uint32 botLow, uint64 epoch, bool anyEpoch) const;
    // M5.
    // The next options for the eligible kinds (ActivityKind::Targets); a kind with an empty target list leaves
    // `eligible`.
    std::vector<ActivityBrain::NextOption> NextOptions(std::vector<ActivityKindId>& eligible, Player* nearPlayer,
                                                       uint64 nowMs, uint32 nowUnix);
    // The chat join requests of this tick (and those held from the last one).
    void JoinRequests(uint64 nowMs, uint32 nowUnix);
    // One request; false: not answered for lack of budget (held for the next tick).
    bool JoinFromChat(ActivityJoinRequest const& request, uint64 nowMs, uint32 nowUnix);
    // Join's choice: the activity `id`, or with 0 the nearest one the player may join; null when none.
    Activity* JoinTarget(Player* player, uint32 id);
    // Join's checks once the activity is chosen, then kind->Join: the player's line.
    std::string JoinChecked(Player* player, Activity& activity);

    // The gates (static: the seams read them without the instance).
    inline static std::atomic<bool> running{false};
    inline static std::atomic<bool> duelRules{false};
    inline static std::atomic<bool> duelAmbient{false};
    inline static std::atomic<bool> restoring{false};

    std::shared_ptr<ActivityConfig const> config;       // the snapshot this tick runs with
    std::array<std::unique_ptr<ActivityKind>, std::size_t(ActivityKindId::Count)> kinds;
    std::map<uint32, std::unique_ptr<Activity>> activities;  // live ones, by id
    std::vector<PendingPlan> plans;
    PendingSchedule schedule;
    std::shared_ptr<BrainSink> organizerSink;           // Organizer jobs' results (drained here)
    ActivityOrganizer organizer;                        // M5: the kinds' own Organizer questions
    ActivityOperations ops;
    ActivityCandidates candidates;
    ActivityCensus census;
    std::vector<Player*> realPlayers;                   // this tick's, rebuilt every tick
    std::unordered_set<uint32> held;                    // participants + reserved, this tick (candidates' filter)
    std::unordered_map<uint32, uint64> botCooldownUntilMs;  // bot guid counter -> BotCooldownMinutes after its last
    std::unordered_map<uint32, uint64> escortRequestAtMs;  // requester guid -> last request, rejected ones included
    std::unordered_map<uint32, uint64> escortCooldownUntilMs;  // requester guid -> actual start's cooldown expiry
    std::unordered_map<uint32, uint32> pendingRestore;  // crash recovery: guid -> prev_ruleset, restored at login
    std::unordered_map<uint32, uint64> lastPublished;   // bot guid counter -> epoch of the last published members
                                                        // (strategy resets for those that appear, go or change)
    std::unordered_map<uint32, uint32> memberActivity;  // bot guid counter -> activity id (duel result routing)
    std::unordered_map<uint32, uint32> storedLeaders;   // activity id -> the leader guid counter the store has
    std::unordered_map<uint32, uint64> groupSinceMs;    // activity id -> first tick its group existed
    // activity id -> bots seen in the activity group once: a bot without a group that was never in it is still
    // waiting for AddMember (FormGroup ran out of budget), not gone
    std::unordered_map<uint32, std::unordered_set<uint32>> inGroupSeen;
    std::map<std::pair<ActivityKindId, uint32>, uint64> ownedRequestAtMs;
    std::map<std::pair<ActivityKindId, uint32>, uint64> ownedCooldownUntilMs;
    std::vector<std::pair<uint32, std::string>> linkedDefenses;
    uint32 onlineBots = 0;                              // random bots in world, counted by CollectRealPlayers
    uint32 nextId = 1;
    uint64 nextPlanId = 1;
    uint64 elapsedMs = 0;                               // since the last full tick
    uint64 nextScheduleMs = 0;
    uint64 nextRetentionMs = 0;
    uint64 nextPruneMs = 0;
    uint64 overLoadSinceMs = 0;                         // AbortAboveMs exceeded since (0: not)
    uint64 lastLoadAbortMs = 0;
    uint64 noPlayerSinceMs = 0;                         // no real player online since (0: one is)
    uint32 lastLoadMs = 0;                              // the load guard's last reading
    bool suspended = false;                             // SuspendAboveMs exceeded on the last tick
    bool paused = false;                                // .playerbots activity pause
    bool dirty = false;                                 // republish at the end of the tick
    bool stopped = false;                               // Shutdown ran
    bool tablesMissingLogged = false;
    std::vector<ActivityJoinRequest> heldRequests;      // M5: chat join requests waiting for budget (<= 10 s)
    std::set<std::pair<uint32, ObjectGuid>> recruitOffers;  // M5: (activity id, player) offers whispered once
    // Counters for List(): started, ended per outcome, operations refused.
    std::array<uint32, std::size_t(ActivityKindId::Count)> cleanRuns{};
    uint32 cleanWaves = 0;
    uint64 startedCount = 0;
    std::array<uint64, 12> endedCount{};
};

#endif
