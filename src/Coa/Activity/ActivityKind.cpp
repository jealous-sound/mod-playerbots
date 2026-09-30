/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityKind.h"
#include "ActivityConfig.h"
#include "CoaText.h"
#include "CoaThread.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Random.h"
#include <cmath>

namespace
{
constexpr float TWO_PI = 6.28318530718f;

// A point on the ground at (x, y) near `aboveZ`, or unset (no ground, or water).
ActivityPoint GroundAt(Map* map, uint32 mapId, float x, float y, float aboveZ)
{
    ActivityPoint point;
    float const ground = map->GetHeight(PHASEMASK_NORMAL, x, y, aboveZ + 10.0f);
    if (ground <= INVALID_HEIGHT || map->IsInWater(PHASEMASK_NORMAL, x, y, ground, DEFAULT_COLLISION_HEIGHT))
        return point;
    point.mapId = mapId;
    point.x = x;
    point.y = y;
    point.z = ground + 0.05f;
    point.zoneId = map->GetZoneId(PHASEMASK_NORMAL, x, y, point.z);
    point.set = true;
    return point;
}
}  // namespace

ActivityMember* Activity::FindMember(ObjectGuid guid)
{
    for (ActivityMember& member : members)
        if (member.guid == guid)
            return &member;
    return nullptr;
}

ActivityMember const* Activity::FindMember(ObjectGuid guid) const
{
    for (ActivityMember const& member : members)
        if (member.guid == guid)
            return &member;
    return nullptr;
}

uint32 Activity::BotsIn() const
{
    uint32 count = 0;
    for (ActivityMember const& member : members)
        if (member.isBot && member.In())
            ++count;
    return count;
}

uint32 Activity::PlayersIn() const
{
    uint32 count = 0;
    for (ActivityMember const& member : members)
        if (!member.isBot && member.In())
            ++count;
    return count;
}

void Activity::SetPhase(ActivityPhase newPhase, uint64 nowMs)
{
    if (phase == newPhase)
        return;
    LOG_DEBUG("playerbots.activity", "activity #{} {}: {} -> {}", id, ActivityKindName(kind), ActivityPhaseName(phase),
              ActivityPhaseName(newPhase));
    phase = newPhase;
    phaseSinceMs = nowMs;
    viewDirty = true;
}

void Activity::SetCenter(ActivityPoint const& point, float newRadius)
{
    if (center.set == point.set && center.mapId == point.mapId && center.x == point.x && center.y == point.y &&
        center.z == point.z && center.zoneId == point.zoneId && radius == newRadius)
        return;
    center = point;
    radius = newRadius;
    viewDirty = true;
}

void Activity::SetTravel(ActivityTravel mode)
{
    if (travel == mode)
        return;
    travel = mode;
    // A new travel rule starts its own bookkeeping: arrival is counted again, stragglers timed again.
    for (ActivityMember& member : members)
    {
        member.stragglerSinceMs = 0;
        if (mode == ActivityTravel::Gather)
            member.arrived = false;
    }
}

void Activity::SetFlags(uint8 flags)
{
    if (viewFlags == flags)
        return;
    viewFlags = flags;
    viewDirty = true;
}

void Activity::SetFollow(ObjectGuid player)
{
    if (follow == player)
        return;
    follow = player;
    viewDirty = true;
    membershipDirty = true;
}

void Activity::SetStyle(uint8 style)
{
    if (viewStyle == style)
        return;
    viewStyle = style;
    viewDirty = true;
}

void Activity::SetClosingEmote(uint32 emote)
{
    if (closingEmote == emote)
        return;
    closingEmote = emote;
    viewDirty = true;
}

void Activity::SetObjective(ObjectGuid target)
{
    if (objective == target)
        return;
    objective = target;
    viewDirty = true;
}

Player* Activity::FollowedPlayer() const
{
    if (follow.IsEmpty())
        return nullptr;
    ActivityMember const* const member = FindMember(follow);
    if (!member || !member->In() || member->isBot)
        return nullptr;
    return ActivityWorld::FindMember(*member);
}

ActivityMember& Activity::Enroll(ObjectGuid guid, uint64 epoch, bool isBot, uint8 level, uint32 nowUnix)
{
    membershipDirty = true;
    if (ActivityMember* existing = FindMember(guid))
    {
        // A real player who left and joined again: the same row, open again.
        if (!existing->In())
        {
            existing->leftUnix = 0;
            existing->leftReason = ActivityLeaveReason::None;
            existing->leftHandled = false;
            existing->joinedUnix = nowUnix;
            existing->epoch = epoch;
            existing->level = level;
            existing->arrived = false;
            existing->deadSinceMs = 0;
            existing->stragglerSinceMs = 0;
        }
        return *existing;
    }

    ActivityMember& member = members.emplace_back();
    member.guid = guid;
    member.epoch = epoch;
    member.isBot = isBot;
    member.role = (!leader.IsEmpty() && guid == leader) ? ActivityRole::Leader : ActivityRole::Member;
    member.level = level;
    member.joinedUnix = nowUnix;
    return member;
}

void Activity::MarkLeft(ObjectGuid guid, ActivityLeaveReason reason, uint32 nowUnix)
{
    ActivityMember* member = FindMember(guid);
    if (!member || !member->In())
        return;
    member->leftUnix = nowUnix ? nowUnix : 1;
    member->leftReason = reason;
    membershipDirty = true;
}

void Activity::Finish(ActivityOutcome endOutcome, std::string endSummary)
{
    if (finishing)
        return;
    finishing = true;
    outcome = endOutcome;
    summary = std::move(endSummary);
    CoaText::CutUtf8Bytes(summary, 255);
    LOG_DEBUG("playerbots.activity", "activity #{} {}: finishing ({})", id, ActivityKindName(kind),
              ActivityOutcomeName(endOutcome));
}

bool Activity::CanPost(ActivityConfig const& config, uint64 nowMs) const
{
    if (posts >= config.chatMaxPosts)
        return false;
    return !lastPostMs || nowMs >= lastPostMs + uint64(config.chatMinGapSeconds) * 1000;
}

void Activity::NotePost(uint64 nowMs)
{
    ++posts;
    lastPostMs = nowMs;
}

namespace ActivityWorld
{
Player* FindMember(ActivityMember const& member)
{
    Player* const player = ObjectAccessor::FindConnectedPlayer(member.guid);
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld())
        return nullptr;
    if (member.isBot)
    {
        PlayerbotAI* const ai = GET_PLAYERBOT_AI(player);
        if (!ai || ai->GetCoaEpoch() != member.epoch)
            return nullptr;
    }
    return player;
}

ActivityPoint PointOf(Player* player)
{
    ActivityPoint point;
    if (!player)
        return point;
    point.mapId = player->GetMapId();
    point.x = player->GetPositionX();
    point.y = player->GetPositionY();
    point.z = player->GetPositionZ();
    point.zoneId = player->GetZoneId();
    point.set = true;
    return point;
}

ActivityPoint GroundNear(ActivityPoint const& center, float spread)
{
    COA_ASSERT_WORLD_THREAD();
    if (!center.set)
        return center;
    Map* const map = sMapMgr->CreateBaseMap(center.mapId);
    if (!map)
        return ActivityPoint();

    if (spread > 0.0f)
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            float const angle = float(rand_norm()) * TWO_PI;
            float const distance = (0.3f + 0.7f * float(rand_norm())) * spread;
            ActivityPoint const point = GroundAt(map, center.mapId, center.x + distance * std::cos(angle),
                                                 center.y + distance * std::sin(angle), center.z);
            if (point.set)
                return point;
        }
    }

    ActivityPoint point = GroundAt(map, center.mapId, center.x, center.y, center.z);
    if (point.set && center.zoneId)
        point.zoneId = center.zoneId;
    return point;
}

bool RealPlayerNear(std::vector<Player*> const& realPlayers, ActivityPoint const& point, float range)
{
    if (!point.set)
        return false;
    float const rangeSq = range * range;
    for (Player* player : realPlayers)
    {
        if (!player || !player->IsInWorld() || player->GetMapId() != point.mapId)
            continue;
        if (point.SqDist2d(player->GetPositionX(), player->GetPositionY()) <= rangeSq)
            return true;
    }
    return false;
}

std::string Line(char const* name, char const* english, std::map<std::string, std::string> const& placeholders)
{
    return PlayerbotTextMgr::instance().GetBotTextOrDefault(name, english, placeholders);
}

std::string AreaName(uint32 areaOrZoneId)
{
    if (!areaOrZoneId)
        return "";
    AreaTableEntry const* const entry = sAreaTableStore.LookupEntry(areaOrZoneId);
    return entry ? PlayerbotAI::GetLocalizedAreaName(entry) : std::string();
}

std::string FirstName(Player* player)
{
    if (!player)
        return "";
    std::string const& name = player->GetName();
    return name.substr(0, name.find(' '));
}
}  // namespace ActivityWorld
