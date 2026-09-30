/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaActivityValues.h"
#include "AiObjectContext.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Unit.h"
#include "WorldSession.h"
#include <cfloat>

namespace
{
constexpr float OBJECTIVE_REACH_YARDS = 60.0f;
}

ActivityBotState& CoaActivityBot::Refresh(PlayerbotAI* botAI)
{
    ActivityBotState& s = botAI->GetAiObjectContext()->GetValue<ActivityBotState&>("coa activity state")->Get();

    // The version is 0 until the first publish, as is s.version: a bot never reads an empty registry for nothing.
    uint64 const v = ActivityRegistry::Version();
    if (v != s.version)
    {
        s.snapshot = ActivityRegistry::Instance().Current();
        s.version = s.snapshot->version;
        ActivityMemberView const* row = s.snapshot->Find(botAI->GetBot()->GetGUID().GetCounter());
        s.row = row && row->epoch == botAI->GetCoaEpoch() ? row : nullptr;
    }

    return s;
}

float CoaActivityBot::DistanceToCenter(Player* bot, ActivityMemberView const& row)
{
    if (!row.center.IsSet() || row.center.mapId != bot->GetMapId())
        return FLT_MAX;

    return bot->GetExactDist2d(row.center.x, row.center.y);
}

bool CoaActivityBot::IsFollowing(ActivityMemberView const& row)
{
    return row.leaderLow != 0 && row.role == ActivityRole::Member;
}

Player* CoaActivityBot::LeaderOnMap(Player* bot, ActivityMemberView const& row)
{
    if (!row.leaderLow)
        return nullptr;

    Player* leader = ObjectAccessor::GetPlayer(*bot, ObjectGuid::Create<HighGuid::Player>(row.leaderLow));
    return leader != bot ? leader : nullptr;
}

ActivityPoint CoaActivityBot::PointOf(Player* player)
{
    ActivityPoint point;
    point.mapId = player->GetMapId();
    point.x = player->GetPositionX();
    point.y = player->GetPositionY();
    point.z = player->GetPositionZ();
    point.set = true;
    return point;
}

bool CoaActivityBot::HasRealSession(Player* player)
{
    return player->GetSession() && !player->GetSession()->IsBot();
}

Unit* CoaActivityBot::ObjectiveInReach(PlayerbotAI* botAI, ActivityMemberView const& row)
{
    if (row.objective.IsEmpty())
        return nullptr;

    Player* bot = botAI->GetBot();
    Unit* unit = botAI->GetUnit(row.objective);
    if (!unit || !unit->IsInWorld() || !unit->IsAlive() || unit->GetMap() != bot->GetMap() ||
        !bot->IsValidAttackTarget(unit) || bot->GetExactDist2d(unit) > OBJECTIVE_REACH_YARDS)
        return nullptr;

    return unit;
}
