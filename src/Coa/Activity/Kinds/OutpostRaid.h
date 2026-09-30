/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_OUTPOSTRAID_H
#define PLAYERBOTS_OUTPOSTRAID_H

#include "ActivityGuards.h"
#include "ActivityKind.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// A line the raid's leader still has to say (retried while the budget is short or the leader fights).
enum class RaidLine : uint8
{
    None = 0,
    Rally,    // coa_act_raid_rally (Party), entering Gathering
    Charge,   // coa_act_raid_charge (Yell), entering Active
    Retreat,  // coa_act_raid_retreat (Yell), on a retreat
    Win,      // coa_act_raid_win (Yell), entering Resolving after the kill
    Lead,     // coa_act_raid_lead (Whisper to the player), entering Disbanding
};

// An outpost raid's own state (ActivityPlan::state, then Activity::state).
struct OutpostRaidState : ActivityKindState
{
    std::string targetName;          // ActivityRaidTarget::name
    ActivitySpawnInfo spawn;         // the quest giver's spawn
    uint8 defenders = 0;             // the defending TeamId (the other one)
    ActivityPoint rally;             // the target's rally (or RAID_RALLY_FALLBACK_YARDS away), on the ground
    ObjectGuid targetGuid;           // the live creature's guid once found
    uint64 formSinceMs = 0;          // Recruiting: FormGroup began (FORM_TIMEOUT_MS)
    uint64 recruitUntilMs = 0;       // Recruiting ends here (Raid.RecruitMinutes)
    uint64 nextPostMs = 0;
    std::string lineKey;             // the recruitment text name of the next post, "" to choose
    uint64 formingUntilMs = 0;       // Forming (recruitment closed) ends here
    uint64 gatherUntilMs = 0;
    uint64 timeoutAtMs = 0;          // recruitment end + Raid.TimeoutMinutes: not killed by then -> Resolving
    uint64 engagedSinceMs = 0;
    uint64 nextRetreatAskMs = 0;     // Active: the retreat question every RETREAT_ASK_MS
    uint64 guardHoldSinceMs = 0;     // a guard holds the raid back (defender near, quest holder) since (0: none)
    uint32 retreats = 0;
    bool iconSet = false;
    bool won = false;
    uint64 nextLeaveMs = 0;          // Disbanding: the next bot leaves the group then
    ActivityOutcome endOutcome = ActivityOutcome::Success;  // what Disbanding finishes with
    std::string endWhy;
    std::vector<ObjectGuid> toAdd;   // accepted bots FormGroup could not add yet (budget): AddMember later
    RaidLine lineDue = RaidLine::None;
    uint64 lineDueUntilMs = 0;
    // Owner additions (hunt-raid).
    bool engaged = false;            // Active: the target was seen fighting or hurt (a despawn then counts as a kill)
    bool guardRetreat = false;       // Gathering: the defender guard pulled the raid back from the march or the fight
    bool emotePending = false;       // Resolving: the closing emote question is out; CELEBRATE once it is read
    uint64 retreatAskedMs = 0;       // the retreat question went out then (an answer from before this engagement is
                                     // stale)
    ObjectGuid leadTo;               // Disbanding: the real player given the lead (coa_act_raid_lead)
    ObjectGuid skullOn;              // the creature the raid's skull marks (empty: none), taken off when Active is left
};

// "raid" (M5, Raid.Enable = 0 by default): 6-15 bots of level Raid.MinLevel and above of the attacking team raid a
// Hillsbrad quest giver (Magistrate Henry Maleb in Southshore for the Horde, High Executor Darthalia in Tarren Mill for
// the Alliance). Phases RECRUITING (the group forms at once, the leader posts coa_act_lfm_raid "in %minutes min,
// %count/%max", real players join by whisper or command), FORMING (recruitment closed, late bots added, raid size
// checked), GATHER (rally), MARCH, ENGAGED (Active: skull, objective), RESOLVED (Resolving) and DISBANDING (the lead to
// a real player member, then the bots leave one by one). Ambient raids plan only when the census gate is open (the
// p10 of the attacking team's raid pool at this hour >= 1.5 x Raid.MinBots), no real defender is in the zone, no real
// player holds a quest from the target, and fewer than Raid.MaxPerTargetPerDay raids hit it this UTC day. No
// engagement while a real defender is within Raid.QuestGiverGuardYards. Contract section 8.
class OutpostRaidKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::Raid; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    void OnEnd(ActivityServices& services, Activity& activity) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;
    std::optional<std::vector<std::string>> Targets(ActivityServices& services,
                                                    ActivityStartRequest const& request) override;
    bool Accepts(ActivityServices& services, Activity const& activity, Player* player) override;

private:
    // (target name, BrainClock::UtcDay) -> raids begun (Raid.MaxPerTargetPerDay). RAM only: a restart forgets it.
    std::map<std::pair<std::string, uint32>, uint32> startsPerDay;
    // Target name -> CoaTime::NowMs() of its last raid's start: the dice take the least recently raided target.
    std::unordered_map<std::string, uint64> lastRaidMs;
    // Target name -> the live raid on it (one raid per target).
    std::unordered_map<std::string, uint32> targetInUse;
};

std::unique_ptr<ActivityKind> CreateOutpostRaidKind();

#endif
