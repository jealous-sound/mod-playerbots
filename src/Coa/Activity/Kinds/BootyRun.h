/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOOTYRUN_H
#define PLAYERBOTS_BOOTYRUN_H

#include "ActivityKind.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <memory>
#include <string>

// A booty run's own state (ActivityPlan::state, then Activity::state).
struct BootyRunState : ActivityKindState
{
    ActivityPoint arena;        // the arena's center (where Short John Mithril puts the chest), z on the ground
    uint32 areaId = 0;          // the area at the arena's center (%area)
    uint64 activeUntilMs = 0;   // Active ends here (Gurubashi.Minutes)
    bool chestSeen = false;     // a participant saw the Arena Treasure Chest once
    uint64 chestSeenMs = 0;     // when (the cheer is retried LINE_RETRY_MS from then)
    bool chestGone = false;     // seen, then gone (someone opened it): Resolving
    uint64 nextChestCheckMs = 0;
    bool called = false;        // coa_act_booty_call posted (or skipped)
    bool cheered = false;       // coa_act_booty_cheer said once the chest is up
    // Owner additions (M5 fixes): with Gurubashi.BotsMayWin = 0 and the chest maybe up, the run ends by walking its
    // bots out of the chest's reach (Gathering at `exit`) before Resolving, so none gets "loot" back beside it.
    ActivityPoint exit;         // Short John Mithril's post on the arena's upper ring, 58 yards from the chest
    bool leaving = false;       // walking to `exit` (phase Gathering after Active)
    uint64 leaveUntilMs = 0;    // the walk out ends here at the latest
};

// "gurubashi" (M5): while game_event 16 (the Gurubashi Arena Booty Run) is active, 4-8 bots of both factions of level
// Gurubashi.MinLevel and above, near a real player's level, gather in the Gurubashi Arena for Gurubashi.Minutes: they
// roar, flex and cheer ("coa party", ACTIVITY_STYLE_ARENA) and fight each other with their stock PvP strategies (the
// arena is free-for-all), and never take the chest with Gurubashi.BotsMayWin = 0 (no "loot" nor "gather" for them).
// It ends when the chest was opened or the time is up. Solo participants, no group. Contract section 9.3.
class BootyRunKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::Gurubashi; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;
};

std::unique_ptr<ActivityKind> CreateBootyRunKind();

#endif
