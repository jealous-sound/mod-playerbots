/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CITYLOITER_H
#define PLAYERBOTS_CITYLOITER_H

#include "ActivityKind.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <memory>
#include <string>
#include <unordered_map>

struct ActivitySpot;

// A city crowd's own state (ActivityPlan::state, then Activity::state).
struct CityLoiterState : ActivityKindState
{
    std::string spotName;       // Loiter.Spots name (a capital's bank by default)
    ActivityPoint spot;         // z on the ground, zone set
    uint32 areaId = 0;          // the area at the spot (%area)
    uint64 activeUntilMs = 0;   // Active ends here (Loiter.Minutes +- 10)
    uint32 mountLines = 0;      // coa_act_loiter_mount said (at most LOITER_MOUNT_LINES_MAX, a real player near)
    uint64 nextMountLineMs = 0;
    uint64 emptySinceMs = 0;    // Active: no real player of the team within LOITER_NEAR_YARDS since (0: someone is)
};

// "loiter" (M5): 3-8 bots (never more than LOITER_MAX_BOTS per city) of level Loiter.MinLevel and above stand around a
// capital's bank for Loiter.Minutes +- 10 while a real player of their team is in that city or near it: idle emotes,
// mounts on show (the "coa party" strategy, ACTIVITY_STYLE_LOITER), now and then a show-off line when a real player
// stands close. One crowd per city. Solo participants, no group. Contract section 9.2.
class CityLoiterKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::Loiter; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    void OnEnd(ActivityServices& services, Activity& activity) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;

private:
    // A spot's check (ground height), cached by its whole definition as DuelGatheringKind does; one LOG_WARN per bad
    // definition.
    struct SpotCheck
    {
        bool good = false;
        ActivityPoint point;
        uint32 areaId = 0;
    };
    SpotCheck const& Check(ActivitySpot const& spot);

    std::unordered_map<std::string, SpotCheck> spotChecks;
    // City zone id -> the live crowd there (one per city).
    std::unordered_map<uint32, uint32> cityInUse;
};

std::unique_ptr<ActivityKind> CreateCityLoiterKind();

#endif
