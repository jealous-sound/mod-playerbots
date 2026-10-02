/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYGUARDS_H
#define PLAYERBOTS_ACTIVITYGUARDS_H

#include "ActivityTypes.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <unordered_set>
#include <vector>

class Creature;
class Player;

// A creature spawn as the hunts and raids see it (M5): from the world data, looked up again at each use.
struct ActivitySpawnInfo
{
    uint32 spawnId = 0;        // creature.guid
    uint32 entry = 0;          // creature_template.entry
    ActivityPoint point;       // the spawn position, zone looked up on the base map
    uint8 level = 0;           // creature_template.maxlevel
    uint32 respawnSeconds = 0; // creature.spawntimesecs
    uint32 npcFlags = 0;       // creature_template.npcflag
};

// Targets and guards of the M5 hunts and raids. World thread only, inside the director's tick (the map threads are
// idle): they read Players, their quest logs, the object manager and the continents' spawn stores.
namespace ActivityGuards
{
// The spawn's data (ObjectMgr::GetCreatureData, GetCreatureTemplate); false when the world does not know it.
bool DescribeSpawn(uint32 spawnId, ActivitySpawnInfo& out);

// The live creature of a spawn on its continent (Map::GetCreatureBySpawnIdStore of the base map), alive or dead;
// null when its grid is not loaded (nobody near) or the spawn is unknown.
Creature* FindSpawn(uint32 spawnId);

// The quest-target guard: a player of `players` (not in `exempt`, guid counters of the activity's own real members)
// has a quest in its log, incomplete, that needs `entry`: a RequiredNpcOrGo kill or cast credit not complete yet, or
// an item that entry drops for it (LootTemplates_Creature.HaveQuestLootForPlayer of the template's lootid). `who`
// gets that player (may be null).
bool QuestTargetNeeded(uint32 entry, std::vector<Player*> const& players, std::unordered_set<uint32> const* exempt,
                       Player** who = nullptr);

// The quest-giver guard: a player of `players` has a quest in its log (incomplete, or complete and not turned in)
// that `entry` starts or ends (GetCreatureQuestRelationBounds, GetCreatureQuestInvolvedRelationBounds).
bool QuestGiverHeld(uint32 entry, std::vector<Player*> const& players);

// A player of `players` of `team` in `zoneId` of `mapId` (the defender-in-zone guard).
bool TeamInZone(std::vector<Player*> const& players, uint8 team, uint32 mapId, uint32 zoneId);

// A player of `players` of `team` within `yards` (2D) of `point`, same map (the defender-near guard).
bool TeamNear(std::vector<Player*> const& players, uint8 team, ActivityPoint const& point, float yards);

// The creature's name in the realm's text locale (PlayerbotAI::GetLocalizedCreatureName), the template's otherwise.
std::string CreatureName(uint32 entry);

// A point `yards` from `from` toward `toward` (2D), on the ground of the base map (Map::GetHeight, not in water);
// unset when no ground is found there nor within 4 tries of +-15 yards around it. Rally points.
ActivityPoint GroundToward(ActivityPoint const& from, float towardX, float towardY, float yards);
}  // namespace ActivityGuards

#endif
