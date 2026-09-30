/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityMoveFarTo.h"
#include "CoaActivityValues.h"
#include "LastMovementValue.h"
#include "Log.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "G3D/Vector3.h"
#include <cfloat>
#include <cmath>

bool ActivityBaseAction::ActivityMoveFarTo(ActivityPoint const& point)
{
    // Cross-map destinations are the director's (a teleport under the visibility rule).
    if (!point.IsSet() || point.mapId != bot->GetMapId())
        return false;

    WorldPosition const dest(point.mapId, point.x, point.y, point.z);

    // Don't start a ground move while the bot is on a taxi (as NewRpgBaseAction::MoveFarTo).
    if (bot->IsInFlight())
        return true;

    ActivityBotState& state = AI_VALUE(ActivityBotState&, "coa activity state");
    if (!state.moveFarDest.IsSet() || state.moveFarDest.mapId != point.mapId ||
        dest.GetExactDist(state.moveFarDest.x, state.moveFarDest.y, state.moveFarDest.z) > 1.0f)
    {
        // clear stuck information if it's a new dest
        state.moveFarDest = point;
        state.nearestMoveFarDis = FLT_MAX;
        state.stuckTs = 0;
        state.stuckAttempts = 0;
        state.stuck = false;
    }

    // performance optimization
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return false;

    // Let previously committed movement finish before recomputing (see NewRpgBaseAction::MoveFarTo).
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    // stuck check: no teleport here. The director's straggler and leader-progress rules teleport under the
    // visibility rule (no real player near the bot or the landing point); the bot keeps trying meanwhile.
    float disToDest = bot->GetDistance(dest);
    if (disToDest + ActivityThreshold::LEADER_PROGRESS_YARDS < state.nearestMoveFarDis)
    {
        state.nearestMoveFarDis = disToDest;
        state.stuckTs = getMSTime();
        state.stuckAttempts = 0;
        state.stuck = false;
    }
    else if (++state.stuckAttempts >= 5 && GetMSTimeDiffToNow(state.stuckTs) >= stuckTime)
    {
        state.stuckTs = getMSTime();
        state.stuckAttempts = 0;
        if (!state.stuck)
            LOG_DEBUG("playerbots.activity", "{} is stuck {:.0f} yards from ({:.1f}, {:.1f}, {:.1f}) on map {}",
                      bot->GetName(), disToDest, point.x, point.y, point.z, point.mapId);
        state.stuck = true;
    }

    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
        return MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false, false,
                      false, true);
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // Primary strategy: ask mmap for a route to the TRUE destination and walk to the furthest reachable waypoint.
    {
        PathGenerator path(bot);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));
        if (canReach)
        {
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            // Only commit if the mmap endpoint actually makes progress toward the destination.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            if (endDistToDest + 5.0f < disToDest)
            {
                return MoveTo(bot->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, true);
            }
        }
    }

    // Fallback: mmap couldn't route to the destination. Sample the forward cone for a reachable stepping stone.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx = 0.0f;
    float ry = 0.0f;
    float rz = 0.0f;
    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            G3D::Vector3 const& endPos = path.GetActualEndPosition();
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);
    }
    return false;
}

ActivityPoint ActivityBaseAction::Jitter(ActivityPoint const& center, float spread)
{
    Map* map = bot->GetMap();
    if (!center.IsSet() || !map || center.mapId != bot->GetMapId() || spread <= 0.0f)
        return center;

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        float const angle = float(rand_norm()) * 2.0f * float(M_PI);
        float const distance = (0.3f + 0.7f * float(rand_norm())) * spread;
        float const px = center.x + std::cos(angle) * distance;
        float const py = center.y + std::sin(angle) * distance;
        float const ground = map->GetHeight(bot->GetPhaseMask(), px, py, center.z + 10.0f);
        if (ground <= INVALID_HEIGHT)
            continue;
        if (map->IsInWater(bot->GetPhaseMask(), px, py, ground, bot->GetCollisionHeight()))
            continue;

        ActivityPoint point = center;
        point.x = px;
        point.y = py;
        point.z = ground + 0.05f;
        return point;
    }

    return center;
}

ActivityPoint ActivityBaseAction::StableJitter(ActivityPoint const& center, float spread, ActivityPhase phase)
{
    ActivityBotState& state = AI_VALUE(ActivityBotState&, "coa activity state");

    bool redraw = !state.jitterFor.IsSet() || !state.jitterTarget.IsSet() || state.jitterPhase != phase ||
                  state.jitterFor.mapId != center.mapId ||
                  state.jitterFor.SqDist2d(center.x, center.y) > 1.0f || std::fabs(state.jitterFor.z - center.z) > 1.0f;
    if (!redraw && state.jitterTarget.mapId == bot->GetMapId() &&
        bot->GetExactDist(state.jitterTarget.x, state.jitterTarget.y, state.jitterTarget.z) <= 3.0f)
        redraw = true;

    if (redraw)
    {
        state.jitterTarget = Jitter(center, spread);
        state.jitterFor = center;
        state.jitterPhase = phase;
    }

    return state.jitterTarget;
}
