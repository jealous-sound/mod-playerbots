/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityGuards.h"
#include "CoaThread.h"
#include "Creature.h"
#include "CreatureData.h"
#include "LootMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "Random.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace
{
// The ground is looked for from these heights above the start point (and this deep), lowest first: a roof, a tree or
// a cave's ceiling below a high start would be taken for the ground.
constexpr std::array<std::pair<float, float>, 3> GROUND_PROBES = {{{10.0f, 100.0f}, {50.0f, 100.0f}, {200.0f, 400.0f}}};
constexpr float AROUND_YARDS = 15.0f;  // the tries around a point without ground
constexpr int AROUND_TRIES = 4;

// The ground at (x, y) on the base map, not in water, probed from `aboveZ` down `depth` yards; unset otherwise.
ActivityPoint GroundAt(Map* map, uint32 mapId, float x, float y, float aboveZ, float depth)
{
    ActivityPoint point;
    float const ground = map->GetHeight(PHASEMASK_NORMAL, x, y, aboveZ, true, depth);
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

ActivityPoint GroundProbe(Map* map, uint32 mapId, float x, float y, float fromZ)
{
    for (auto const& [above, depth] : GROUND_PROBES)
    {
        ActivityPoint const point = GroundAt(map, mapId, x, y, fromZ + above, depth);
        if (point.set)
            return point;
    }
    return ActivityPoint();
}

bool Counts(Player* player) { return player && player->IsInWorld() && !player->IsDuringRemoveFromWorld(); }
}  // namespace

namespace ActivityGuards
{
bool DescribeSpawn(uint32 spawnId, ActivitySpawnInfo& out)
{
    COA_ASSERT_WORLD_THREAD();
    CreatureData const* const data = spawnId ? sObjectMgr->GetCreatureData(spawnId) : nullptr;
    if (!data)
        return false;
    CreatureTemplate const* const ct = sObjectMgr->GetCreatureTemplate(data->id);
    if (!ct)
        return false;

    out = ActivitySpawnInfo();
    out.spawnId = spawnId;
    out.entry = data->id;
    out.point.mapId = data->mapid;
    out.point.x = data->posX;
    out.point.y = data->posY;
    out.point.z = data->posZ;
    out.point.set = true;
    if (Map* const map = sMapMgr->CreateBaseMap(data->mapid))
        out.point.zoneId = map->GetZoneId(PHASEMASK_NORMAL, data->posX, data->posY, data->posZ);
    out.level = ct->maxlevel;
    out.respawnSeconds = data->spawntimesecs;
    out.npcFlags = ct->npcflag;
    return true;
}

Creature* FindSpawn(uint32 spawnId)
{
    COA_ASSERT_WORLD_THREAD();
    CreatureData const* const data = spawnId ? sObjectMgr->GetCreatureData(spawnId) : nullptr;
    if (!data)
        return nullptr;
    Map* const map = sMapMgr->FindBaseNonInstanceMap(data->mapid);
    if (!map)
        return nullptr;

    Creature* dead = nullptr;
    auto const range = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
    for (auto it = range.first; it != range.second; ++it)
    {
        Creature* const creature = it->second;
        if (!creature || !creature->IsInWorld())
            continue;
        if (creature->IsAlive())
            return creature;
        if (!dead)
            dead = creature;
    }
    return dead;
}

bool QuestTargetNeeded(uint32 entry, std::vector<Player*> const& players, std::unordered_set<uint32> const* exempt,
                       Player** who)
{
    COA_ASSERT_WORLD_THREAD();
    if (who)
        *who = nullptr;
    if (!entry)
        return false;
    CreatureTemplate const* const ct = sObjectMgr->GetCreatureTemplate(entry);
    uint32 const lootId = ct ? ct->lootid : 0;

    for (Player* player : players)
    {
        if (!Counts(player) || (exempt && exempt->count(player->GetGUID().GetCounter())))
            continue;

        bool needs = false;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && !needs; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            if (!questId || player->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* const quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                if (quest->RequiredNpcOrGo[i] == int32(entry) &&
                    player->GetQuestSlotCounter(slot, i) < quest->RequiredNpcOrGoCount[i])
                {
                    needs = true;
                    break;
                }
            }
        }
        // A quest item the target drops for this player (Hogger's claw, Gallywix's head).
        if (!needs && lootId && LootTemplates_Creature.HaveQuestLootForPlayer(lootId, player))
            needs = true;

        if (needs)
        {
            if (who)
                *who = player;
            return true;
        }
    }
    return false;
}

bool QuestGiverHeld(uint32 entry, std::vector<Player*> const& players)
{
    COA_ASSERT_WORLD_THREAD();
    if (!entry || players.empty())
        return false;

    std::vector<uint32> quests;
    for (QuestRelationBounds bounds :
         {sObjectMgr->GetCreatureQuestRelationBounds(entry), sObjectMgr->GetCreatureQuestInvolvedRelationBounds(entry)})
        for (auto it = bounds.first; it != bounds.second; ++it)
            quests.push_back(it->second);
    if (quests.empty())
        return false;
    std::sort(quests.begin(), quests.end());
    quests.erase(std::unique(quests.begin(), quests.end()), quests.end());

    for (Player* player : players)
    {
        if (!Counts(player))
            continue;
        for (uint32 const questId : quests)
        {
            QuestStatus const status = player->GetQuestStatus(questId);
            if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
                return true;
        }
    }
    return false;
}

// A game master in GM mode is nobody's defender: invisible to the NPCs and to the bots.
bool TeamInZone(std::vector<Player*> const& players, uint8 team, uint32 mapId, uint32 zoneId)
{
    COA_ASSERT_WORLD_THREAD();
    for (Player* player : players)
    {
        if (!Counts(player) || player->IsGameMaster() || uint8(player->GetTeamId()) != team)
            continue;
        if (player->GetMapId() == mapId && player->GetZoneId() == zoneId)
            return true;
    }
    return false;
}

bool TeamNear(std::vector<Player*> const& players, uint8 team, ActivityPoint const& point, float yards)
{
    COA_ASSERT_WORLD_THREAD();
    if (!point.set)
        return false;
    float const yardsSq = yards * yards;
    for (Player* player : players)
    {
        if (!Counts(player) || player->IsGameMaster() || uint8(player->GetTeamId()) != team ||
            player->GetMapId() != point.mapId)
            continue;
        if (point.SqDist2d(player->GetPositionX(), player->GetPositionY()) <= yardsSq)
            return true;
    }
    return false;
}

std::string CreatureName(uint32 entry) { return PlayerbotAI::GetLocalizedCreatureName(entry); }

ActivityPoint GroundToward(ActivityPoint const& from, float towardX, float towardY, float yards)
{
    COA_ASSERT_WORLD_THREAD();
    if (!from.set)
        return ActivityPoint();
    Map* const map = sMapMgr->CreateBaseMap(from.mapId);
    if (!map)
        return ActivityPoint();

    float dx = towardX - from.x;
    float dy = towardY - from.y;
    float const length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.01f)
    {
        dx = 1.0f;
        dy = 0.0f;
    }
    else
    {
        dx /= length;
        dy /= length;
    }
    float const x = from.x + dx * yards;
    float const y = from.y + dy * yards;

    ActivityPoint point = GroundProbe(map, from.mapId, x, y, from.z);
    for (int attempt = 0; attempt < AROUND_TRIES && !point.set; ++attempt)
        point = GroundProbe(map, from.mapId, x + frand(-AROUND_YARDS, AROUND_YARDS),
                            y + frand(-AROUND_YARDS, AROUND_YARDS), from.z);
    return point;
}
}  // namespace ActivityGuards
