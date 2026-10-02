/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ESCORT_H
#define PLAYERBOTS_ESCORT_H

#include "ActivityKind.h"
#include <cmath>
#include <limits>
#include <memory>

namespace EscortRules
{
enum Hazard : uint32
{
    Offline = 1 << 0,
    Dead = 1 << 1,
    Instance = 1 << 2,
    Flight = 1 << 3,
    Teleport = 1 << 4,
    Pvp = 1 << 5,
    Ffa = 1 << 6,
    WarMode = 1 << 7,
    HighRisk = 1 << 8,
    Mercenary = 1 << 9,
    Duel = 1 << 10,
    Battleground = 1 << 11,
    Lfg = 1 << 12,
};

// Plain snapshots of the world-thread checks: useful to verify every excluded ruleset without Player objects.
inline char const* WhyUnfit(uint32 hazards, uint32 mapId, uint32 destinationMap)
{
    if (hazards & Offline)
        return "offline";
    if (hazards & Dead)
        return "dead";
    if ((hazards & Instance) || mapId != destinationMap)
        return "left the escort's overworld map";
    if (hazards & (Flight | Teleport))
        return "in flight or teleporting";
    if (hazards & (Pvp | Ffa | WarMode | HighRisk | Mercenary))
        return "not in PvE";
    if (hazards & (Duel | Battleground | Lfg))
        return "in a duel, battleground, or queue";
    return nullptr;
}

inline bool CanAcceptInvitation(bool invited, bool accepted, uint64 nowMs, uint64 deadlineMs)
{
    return invited && !accepted && deadlineMs && nowMs < deadlineMs;
}

inline bool Compatible(uint32 botTeam, uint32 requesterTeam, uint32 botLevel, uint32 requesterLevel, uint32 spread)
{
    return botTeam == requesterTeam &&
           (botLevel >= requesterLevel ? botLevel - requesterLevel : requesterLevel - botLevel) <= spread;
}

// Pure, also used by the offline harness: another map, another floor, or invalid coordinates never arrive.
inline bool Arrived(ActivityPoint const& destination, uint32 mapId, float x, float y, float z, float radius)
{
    if (!destination.set || destination.mapId != mapId || !std::isfinite(destination.x) ||
        !std::isfinite(destination.y) || !std::isfinite(destination.z) || !std::isfinite(x) ||
        !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(radius) || radius <= 0.0f)
        return false;
    double const dx = double(x) - destination.x;
    double const dy = double(y) - destination.y;
    double const dz = double(z) - destination.z;
    return dx * dx + dy * dy + dz * dz <= double(radius) * radius;
}

// Callable by the offline harness; boundaries include a different floor, each hazardous state, late acceptance,
// and duplicate acceptance (which must never extend the duration).
inline bool SelfTest()
{
    ActivityPoint point;
    point.mapId = 0;
    point.x = 10.0f;
    point.y = 20.0f;
    point.z = 30.0f;
    point.set = true;
    if (!Arrived(point, 0, 13.0f, 24.0f, 30.0f, 5.0f) ||
        Arrived(point, 0, 13.0f, 24.0f, 30.1f, 5.0f) || Arrived(point, 1, 10.0f, 20.0f, 30.0f, 5.0f) ||
        Arrived(point, 0, 10.0f, 20.0f, 60.0f, 20.0f) ||
        Arrived(point, 0, std::numeric_limits<float>::quiet_NaN(), 20.0f, 30.0f, 5.0f) ||
        Arrived(point, 0, 10.0f, 20.0f, 30.0f, std::numeric_limits<float>::infinity()))
        return false;
    point.set = false;
    if (Arrived(point, 0, 10.0f, 20.0f, 30.0f, 5.0f) || WhyUnfit(0, 0, 0) || !WhyUnfit(0, 1, 0))
        return false;
    for (uint32 hazard = Offline; hazard <= Lfg; hazard <<= 1)
        if (!WhyUnfit(hazard, 0, 0))
            return false;
    if (!Compatible(0, 0, 15, 20, 5) || !Compatible(0, 0, 25, 20, 5) ||
        Compatible(0, 0, 14, 20, 5) || Compatible(0, 0, 26, 20, 5) || Compatible(0, 1, 20, 20, 5) ||
        !Compatible(0, 0, 20, 20, 0) || Compatible(0, 0, 21, 20, 0))
        return false;
    return CanAcceptInvitation(true, false, 999, 1000) && !CanAcceptInvitation(true, false, 1000, 1000) &&
           !CanAcceptInvitation(true, false, 1001, 1000) && !CanAcceptInvitation(false, false, 999, 1000) &&
           !CanAcceptInvitation(true, true, 999, 1000) && !CanAcceptInvitation(true, false, 0, 0);
}
}  // namespace EscortRules

// An explicitly requested overworld escort. The ordinary invitation enrolls the requester; then normal player
// group following and combat handle the walk. This kind never teleports, rewards, summons, or sets an objective.
class EscortKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::Escort; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    bool Accepts(ActivityServices& services, Activity const& activity, Player* player) override;
    bool OnPlayerJoined(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;
};

std::unique_ptr<ActivityKind> CreateEscortKind();

#endif
