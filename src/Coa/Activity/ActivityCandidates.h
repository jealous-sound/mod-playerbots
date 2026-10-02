/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYCANDIDATES_H
#define PLAYERBOTS_ACTIVITYCANDIDATES_H

#include "ActivityTypes.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

// A free random bot, copied into plain values when found (world thread). Nothing keeps the Player.
struct ActivityCandidate
{
    ObjectGuid guid;
    uint64 epoch = 0;        // its PlayerbotAI's CoaEpoch
    uint8 level = 0;
    uint8 classId = 0;
    uint8 team = 0;          // TeamId
    uint32 mapId = 0;
    uint32 zoneId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float distance = -1.0f;  // 2D yards to the query's center on the same map; -1 on another map or without center
    uint32 bravery = 0;      // ActivityTraits
    uint32 sociability = 0;
    uint32 pvpAppetite = 0;
};

struct ActivityCandidateQuery
{
    uint8 team = 0;                      // TeamId
    uint8 minLevel = 1;
    uint8 maxLevel = 255;
    ActivityPoint center;                // ranking; unset: no distance
    bool sameMapOnly = false;            // only bots on center.mapId
    float maxDistance = 0.0f;            // same map only, 0 = any distance
    uint32 limit = 16;                   // at most this many, best first
    bool rulesetSafe = false;            // skip High-Risk (1004019) and Mercenary (9930874) bots (War Mode kinds)
    std::unordered_set<uint32> const* exclude = nullptr;  // guid counters to skip besides the held ones
};

// Finds free random bots for the kinds. World thread only (COA_ASSERT_WORLD_THREAD): one pass over
// sRandomPlayerbotMgr.GetAllBots() per Find, reading each Player while the map threads are idle.
class ActivityCandidates
{
public:
    // The director, at the start of each tick: what counts as held (the published snapshot's members and reserved,
    // plus the director's own not yet published reservations) and the per-bot cooldowns (BotCooldownMinutes).
    // The pointers stay valid for the tick.
    void BeginTick(uint64 nowMs, std::unordered_set<uint32> const* held,
                   std::unordered_map<uint32, uint64> const* cooldownUntilMs);

    // Free bots matching the query, best first: same map as the center first, then by distance, then by level
    // closeness to the middle of [minLevel, maxLevel], then by guid (stable).
    std::vector<ActivityCandidate> Find(ActivityCandidateQuery const& query) const;

    // The hard filters (contract section 7.1) without the held set, the cooldowns and combat: online and in world,
    // not teleporting, alive, ungrouped, no master, overworld, not in a battleground, arena, their queues or the
    // Dungeon Finder, not flying, no duel, a random bot with a PlayerbotAI, no "lfg bot" offer pending. The census
    // counts these (minus the held ones).
    static bool IsFree(Player* bot);

    // IsFree, not in combat, not held, past its cooldown. What Find applies before the query's own filters.
    bool IsAvailable(Player* bot) const;

    // Copies a bot into a candidate (distance to `center` when set and on the same map).
    static ActivityCandidate Describe(Player* bot, ActivityPoint const& center);

private:
    uint64 nowMs = 0;
    std::unordered_set<uint32> const* held = nullptr;
    std::unordered_map<uint32, uint64> const* cooldownUntilMs = nullptr;
};

#endif
