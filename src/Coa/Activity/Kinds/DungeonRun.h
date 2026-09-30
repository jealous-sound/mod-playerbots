/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONRUN_H
#define PLAYERBOTS_DUNGEONRUN_H

#include "ActivityKind.h"
#include "Define.h"
#include "DungeonLfg.h"
#include "ObjectGuid.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Where a dungeon run stands inside its phase (the phase is what the registry and the store see).
enum class DungeonStep : uint8
{
    Prepare = 0,  // Forming: bots of another role get one (DungeonLfg::PrepareRole), one per tick
    Form,         // Forming: FormGroup (leader = the tank when there is one)
    Recruit,      // Recruiting: LFM posts, ACTIVITY_VIEW_RECRUITING, waiting for real players to fill the open roles
    Queue,        // Gathering: the leader queued the group at the Dungeon Finder; role check, queue, proposal
    Walk,         // Marching: no Dungeon Finder (off, failed Dungeon.LfgTries times): the group walks to the portal
    Portal,       // Active: at the portal, waiting for the player; the bots follow the player in
    Inside,       // Active: a real member is in the instance; ACTIVITY_VIEW_QUIET, the bots follow that player
    Idle,         // Resolving: the player left or the run ended; the bots wait Dungeon.IdleDisbandMinutes, then go
};

char const* DungeonStepName(DungeonStep step);  // "prepare", "form", "recruit", "queue", "walk", "portal", ...

// A dungeon run's own state (ActivityPlan::state, then Activity::state).
struct DungeonRunState : ActivityKindState
{
    DungeonInfo dungeon;
    ObjectGuid trigger;                // the real player it was planned for (ungrouped, not queued, in the zone)
    // Plan only, aligned with ActivityPlan::candidates: the role (CoaRole) and seat each would fill, and whether its
    // specialization already fits. Two candidates per seat at most, the seat's first choice first; seat 0 leads.
    std::vector<uint8> candidateRole;
    std::vector<uint8> candidateSeat;
    std::vector<bool> candidateFits;
    std::vector<uint8> seatRoles;      // the bots' seats, seat 0 first
    std::vector<uint8> openRoles;      // roles left for real players (one entry per open seat), shrinks as they join
    std::vector<ObjectGuid> toPrepare; // enrolled bots whose role must be switched before the group forms
    std::vector<uint8> toPrepareRole;  // their roles, same order
    std::vector<ObjectGuid> toAdd;     // accepted bots FormGroup could not add yet (budget): AddMember later
    DungeonStep step = DungeonStep::Prepare;
    uint64 stepSinceMs = 0;
    uint64 recruitUntilMs = 0;         // Recruit: the LFM ends here without a taker (Dungeon.RecruitMinutes)
    uint64 nextPostMs = 0;             // Recruit: the next LFM post (Chat.RecruitEverySeconds, Activity::CanPost)
    std::string lineKey;               // the recruitment text name of the next post (brain or dice), "" to choose
    uint32 lfgTries = 0;               // Dungeon Finder attempts made
    uint64 queueUntilMs = 0;           // Queue: Dungeon.QueueMinutes after the attempt
    uint64 retryAtMs = 0;              // Queue: the next attempt after a failed one (30 s later)
    uint64 enteredMs = 0;              // Inside: when a real member was first seen in the instance
    uint64 outsideSinceMs = 0;         // Inside: no real member in the instance since (0: one is)
    uint64 idleUntilMs = 0;            // Idle: Finish(outcome) here
    bool reachedDungeon = false;       // a real member was inside once
    bool finished = false;             // DungeonQueueState::Finished was seen
    bool queueLineSaid = false;        // coa_act_dungeon_queue said
    bool meetLineSaid = false;         // coa_act_dungeon_meet whispered (the walking fallback)
    ActivityOutcome endOutcome = ActivityOutcome::Success;  // what Idle finishes with
    std::string endWhy;                // its summary

    // DungeonRun.cpp's own bookkeeping.
    std::vector<ObjectGuid> seatBots;  // the enrolled bot of each seat, aligned with seatRoles
    std::vector<ObjectGuid> toClean;   // enrolled bots whose role fits: queues and foreign binds dropped in Prepare
    std::map<ObjectGuid, uint8> heldSeats;    // invited real player -> the open role kept for it
    std::map<ObjectGuid, uint8> playerRoles;  // joined real player -> the open role it filled
    // Recruit: a joined real player who logged out but still holds its group slot -> its role, kept for it (not
    // advertised); enrolled again when it is back in the world, the seat opened when it left the group.
    std::map<ObjectGuid, uint8> awayRoles;
    std::map<ObjectGuid, uint64> enterTeleportMs;  // Inside: bot -> its last teleport to the entrance
    std::set<ObjectGuid> meetTold;     // real members whispered coa_act_dungeon_meet
    uint64 meetUntilMs = 0;            // the meet whisper is given up after this
    uint64 dungeonSeenMs = 0;          // Queue: the finder's state turned Dungeon (the teleport is under way)
    uint8 lineDue = 0;                 // a party line still to say (DungeonRun.cpp's DungeonLine)
    uint64 lineDueUntilMs = 0;         // ... given up after this
    bool postTried = false;            // Recruit: a first post was tried (the trigger's offer follows it)
    bool offerSent = false;            // coa_act_recruit_offer whispered to the trigger (or skipped)
    bool lfgPending = false;           // Queue: an attempt is at the finder (its return to None means it failed)
    bool leadHanded = false;           // the lead went to a real player
};

// "dungeon" (M5): for a real player who is ungrouped, not queued and out in the world, 3-4 bots within
// +-Dungeon.LevelSpread of the player's level, picked by role with FindCoaRecruit (the roles the player does not
// play), form a group; the leader posts coa_act_lfm_dungeon in the zone channels it is on; real players join by
// whisper (a join word to the leader, "!join"), by .playerbots activity join, or after an "lfg bot" request (the
// leader whispers an offer); once the open roles are filled the leader queues the group at the Dungeon Finder, so
// the player accepts the role check and the proposal and is teleported by consent (fallback: the group walks to the
// portal and the player is told where to meet). Inside, the lead goes to the player and the bots follow the player
// (ACTIVITY_VIEW_QUIET). The bots leave Dungeon.IdleDisbandMinutes after the player left or the run ended. Contract
// section 6.
class DungeonRunKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::Dungeon; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    void OnEnd(ActivityServices& services, Activity& activity) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;
    bool Accepts(ActivityServices& services, Activity const& activity, Player* player) override;
    bool OnPlayerJoined(ActivityServices& services, Activity& activity, ActivityMember const& member) override;

private:
    // Real player guid counter -> CoaTime::NowMs() before which no run is planned for that player again
    // (Dungeon.PlayerCooldownMinutes, stamped at Begin). Pruned in Plan.
    std::unordered_map<uint32, uint64> playerCooldownUntilMs;
};

std::unique_ptr<ActivityKind> CreateDungeonRunKind();

#endif
