/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GANGPARTY_H
#define PLAYERBOTS_GANGPARTY_H

#include "ActivityKind.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <memory>
#include <string>
#include <vector>

// One stop of a gang's route: a hunt center in a zone of the gang's level.
struct GangStop
{
    ActivityPoint center;   // a grind spot of sTravelMgr.GetLocsPerLevelCache(level), zone set
    uint32 huntMs = 0;      // time to hunt there once arrived
};

// A line the gang leader still has to say (retried while the budget is short or the leader fights).
enum class GangLine : uint8
{
    None = 0,
    Form,   // coa_act_gang_form, entering Gathering
    Move,   // coa_act_gang_move, entering each Marching
    Brag,   // coa_act_gang_brag, entering Resolving
};

// A gang's own state (Activity::state).
struct GangPartyState : ActivityKindState
{
    std::vector<GangStop> route;    // 2-3 stops
    uint32 stop = 0;                // the stop being walked to or hunted at
    uint64 huntUntilMs = 0;         // Active at `stop` ends here
    uint64 formSinceMs = 0;         // Forming began (FORM_TIMEOUT_MS)
    uint64 gatherUntilMs = 0;       // Gathering ends here at the latest
    uint32 minutes = 0;             // planned length
    bool announced = false;         // coa_act_gang_form said
    std::vector<ObjectGuid> toAdd;  // accepted bots FormGroup could not add yet (budget): AddMember later
    GangLine lineDue = GangLine::None;
    uint64 lineDueUntilMs = 0;      // the due line is dropped after this
};

// "gang": 3-5 bots of one faction within +-Gang.LevelSpread levels form a group led by the bravest, roam 2-3 zones of
// their bracket (hunting with "grind" around each stop) for Gang.Minutes +- 15, then brag and disband. PvE unless
// PvpRuleset = warmode. At most Gang.Max gangs. Real players join by .playerbots activity join. Contract section 13.
class GangPartyKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::Gang; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    void OnEnd(ActivityServices& services, Activity& activity) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;
};

std::unique_ptr<ActivityKind> CreateGangPartyKind();

#endif
