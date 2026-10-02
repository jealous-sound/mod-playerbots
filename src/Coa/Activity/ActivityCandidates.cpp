/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityCandidates.h"
#include "CoaSpecialization.h"
#include "CoaThread.h"
#include "LFGMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "TravelMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

void ActivityCandidates::BeginTick(uint64 now, std::unordered_set<uint32> const* heldSet,
                                   std::unordered_map<uint32, uint64> const* cooldowns)
{
    nowMs = now;
    held = heldSet;
    cooldownUntilMs = cooldowns;
}

bool ActivityCandidates::IsFree(Player* bot)
{
    if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld() || bot->IsBeingTeleported() ||
        !bot->GetSession() || bot->GetSession()->isLogingOut() || !bot->IsAlive() || bot->GetGroup() ||
        bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue() || bot->IsInFlight() || bot->duel)
        return false;

    if (!WorldPosition(bot).isOverworld())
        return false;

    PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);
    if (!ai || ai->GetMaster() || !ai->GetCoaEpoch())
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
        return false;

    return !CoaLfgIsOffered(bot->GetGUID());
}

bool ActivityCandidates::IsAvailable(Player* bot) const
{
    if (!IsFree(bot) || bot->IsInCombat())
        return false;

    uint32 const low = bot->GetGUID().GetCounter();
    if (held && held->count(low))
        return false;

    if (cooldownUntilMs)
    {
        auto const cooldown = cooldownUntilMs->find(low);
        if (cooldown != cooldownUntilMs->end() && cooldown->second > nowMs)
            return false;
    }
    return true;
}

ActivityCandidate ActivityCandidates::Describe(Player* bot, ActivityPoint const& center)
{
    ActivityCandidate candidate;
    candidate.guid = bot->GetGUID();
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);
    candidate.epoch = ai ? ai->GetCoaEpoch() : 0;
    candidate.level = bot->GetLevel();
    candidate.classId = bot->getClass();
    candidate.team = uint8(bot->GetTeamId());
    candidate.mapId = bot->GetMapId();
    candidate.zoneId = bot->GetZoneId();
    candidate.x = bot->GetPositionX();
    candidate.y = bot->GetPositionY();
    candidate.z = bot->GetPositionZ();
    if (center.set && center.mapId == candidate.mapId)
        candidate.distance = std::sqrt(center.SqDist2d(candidate.x, candidate.y));

    uint32 const low = candidate.guid.GetCounter();
    candidate.bravery = ActivityTraits::Bravery(low);
    candidate.sociability = ActivityTraits::Sociability(low);
    candidate.pvpAppetite = ActivityTraits::PvpAppetite(low);
    return candidate;
}

std::vector<ActivityCandidate> ActivityCandidates::Find(ActivityCandidateQuery const& query) const
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<ActivityCandidate> found;
    if (!query.limit)
        return found;

    float const maxDistanceSq = query.maxDistance * query.maxDistance;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (!bot || uint8(bot->GetTeamId()) != query.team)
            continue;

        uint8 const level = bot->GetLevel();
        if (level < query.minLevel || level > query.maxLevel)
            continue;

        uint32 const low = bot->GetGUID().GetCounter();
        if (query.exclude && query.exclude->count(low))
            continue;

        if (query.sameMapOnly && bot->GetMapId() != query.center.mapId)
            continue;

        if (query.maxDistance > 0.0f &&
            (!query.center.set || bot->GetMapId() != query.center.mapId ||
             query.center.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) > maxDistanceSq))
            continue;

        if (!IsAvailable(bot))
            continue;

        if (query.rulesetSafe && (bot->HasAura(ActivityThreshold::SPELL_HIGH_RISK) ||
                                  bot->HasAura(ActivityThreshold::SPELL_MERCENARY)))
            continue;

        found.push_back(Describe(bot, query.center));
    }

    int32 const middle = (int32(query.minLevel) + int32(query.maxLevel)) / 2;
    ActivityPoint const& center = query.center;
    std::sort(found.begin(), found.end(),
              [&center, middle](ActivityCandidate const& a, ActivityCandidate const& b)
              {
                  bool const aSame = center.set && a.mapId == center.mapId;
                  bool const bSame = center.set && b.mapId == center.mapId;
                  if (aSame != bSame)
                      return aSame;
                  bool const aNear = a.distance >= 0.0f;
                  bool const bNear = b.distance >= 0.0f;
                  if (aNear != bNear)
                      return aNear;
                  if (aNear && a.distance != b.distance)
                      return a.distance < b.distance;
                  int32 const aGap = std::abs(int32(a.level) - middle);
                  int32 const bGap = std::abs(int32(b.level) - middle);
                  if (aGap != bGap)
                      return aGap < bGap;
                  return a.guid.GetCounter() < b.guid.GetCounter();
              });

    if (found.size() > query.limit)
        found.resize(query.limit);
    return found;
}
