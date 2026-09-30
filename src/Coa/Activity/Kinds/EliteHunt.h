/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ELITEHUNT_H
#define PLAYERBOTS_ELITEHUNT_H

#include "ActivityGuards.h"
#include "ActivityKind.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// A line the hunt's leader still has to say (retried while the budget is short or the leader fights).
enum class HuntLine : uint8
{
    None = 0,
    Go,   // coa_act_hunt_go, entering Active
    Win,  // coa_act_hunt_win, entering Resolving after the kill
};

// An elite hunt's own state (ActivityPlan::state, then Activity::state).
struct EliteHuntState : ActivityKindState
{
    std::string targetName;        // ActivityHuntTarget::name
    ActivitySpawnInfo spawn;       // the target's spawn (entry, point, level)
    ActivityPoint rally;           // HUNT_RALLY_YARDS from the spawn, on the ground: Gathering's center
    ObjectGuid targetGuid;         // the live creature's guid once found (its runtime guid, not the spawn id)
    uint64 formSinceMs = 0;        // Forming began (FORM_TIMEOUT_MS)
    uint64 recruitUntilMs = 0;     // Recruiting ends here (Hunt.RecruitSeconds)
    uint64 nextPostMs = 0;         // the next LFM post
    std::string lineKey;           // the recruitment text name of the next post (brain or dice), "" to choose
    uint64 gatherUntilMs = 0;      // Gathering ends here at the latest (Travel.GatherTimeoutSeconds)
    uint64 engagedSinceMs = 0;     // Active began
    uint32 retreats = 0;           // wipes survived (the second one ends the hunt)
    bool iconSet = false;          // the skull is on the target
    bool won = false;              // the target died while engaged
    std::vector<ObjectGuid> toAdd; // accepted bots FormGroup could not add yet (budget): AddMember later
    HuntLine lineDue = HuntLine::None;
    uint64 lineDueUntilMs = 0;
    // Owner additions (hunt-raid).
    uint64 nextRetreatCheckMs = 0; // Active: the next wipe check (every RETREAT_CHECK_MS)
    bool engaged = false;          // Active: the target was seen fighting or hurt (a despawn then counts as a kill)
    bool emotePending = false;     // Resolving: the closing emote question is out; CELEBRATE once it is read
    ActivityOutcome endOutcome = ActivityOutcome::Success;  // what Resolving finishes with when not won
    std::string endWhy;
    ObjectGuid skullOn;            // the creature the hunt's skull marks (empty: none), taken off when Active is left
};

// "hunt" (M5): 3-6 bots of the target's team within [level - Hunt.LevelBelow, level + Hunt.LevelAbove] of a named
// elite (Hunt.Targets: Hogger for the Alliance, Grand Foreman Puzik Gallywix for the Horde) form a group, recruit
// real players in the zone channels for Hunt.RecruitSeconds (join words, "!join", .playerbots activity join), gather
// out of the target's reach, walk up to it, mark it with a skull and kill it, then cheer. The quest-target guard
// (ActivityGuards::QuestTargetNeeded) is checked when planning and on every tick until the kill: a real player
// outside the group who needs the target ends the hunt (outcome Guarded) before it dies. Contract section 7.
class EliteHuntKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::Hunt; }
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
    // Target name -> CoaTime::NowMs() of its last hunt's start: the dice take the least recently hunted target.
    std::unordered_map<std::string, uint64> lastHuntMs;
    // Target name -> the live hunt going for it (one hunt per target).
    std::unordered_map<std::string, uint32> targetInUse;
};

std::unique_ptr<ActivityKind> CreateEliteHuntKind();

#endif
