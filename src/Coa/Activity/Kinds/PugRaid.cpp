/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#include "PugRaid.h"
#include "ActivityCensus.h"
#include "ActivityDirector.h"
#include "ActivityOperations.h"
#include "CoaActivityValues.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "LootMgr.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

CampaignSettings PugRaidKind::Settings(ActivityConfig const& config) const
{
    CampaignSettings s;
    s.enabled = config.pugRaidEnable;
    s.minimum = config.pugRaidMinBots;
    s.maximum = config.pugRaidMaxBots;
    s.recruitSeconds = config.pugRaidRecruitSeconds;
    s.minutes = config.pugRaidMinutes;
    s.raid = true;
    s.instance = true;
    s.lowest = s.highest = 60;
    return s;
}

std::vector<ActivityCampaignTarget> const& PugRaidKind::Configured(ActivityConfig const& config) const
{
    return config.pugRaidTargets;
}

bool PugRaidKind::Validate(ActivityServices& services, ActivityStartRequest const& request,
                           ActivityCampaignTarget const& target, CampaignState& state, std::string& why) const
{
    MapEntry const* const raid = sMapStore.LookupEntry(target.raidMap);
    if (!raid || !raid->IsRaid() || !CampaignRules::SupportedRaid(target.raidMap, target.strategy) ||
        !CampaignWorld::ResolveTarget(target, state.spawn, why, true) || state.spawn.point.mapId != target.raidMap)
    {
        why = "raid target requires an actual spawn and the existing MC, BWL, or AQ20 strategy on its raid map";
        return false;
    }
    auto& pug = static_cast<PugRaidState&>(state);
    pug.portalTrigger = 0;
    float nearest = 300.0f * 300.0f;
    for (auto const& entry : sObjectMgr->GetAllAreaTriggerTeleports())
        if (entry.second.target_mapId == target.raidMap)
            if (AreaTrigger const* const portal = sObjectMgr->GetAreaTrigger(entry.first);
                portal && portal->map == target.rally.mapId && std::abs(portal->z - target.rally.z) <= 50.0f)
            {
                float const distance = target.rally.SqDist2d(portal->x, portal->y);
                if (distance < nearest)
                {
                    nearest = distance;
                    pug.portalTrigger = entry.first;
                    pug.portal = target.rally;
                    pug.portal.x = portal->x;
                    pug.portal.y = portal->y;
                    pug.portal.z = portal->z;
                }
            }
    if (!pug.portalTrigger)
    {
        why = "raid requires a real native portal on the explicit overworld rally map within 300 yards";
        return false;
    }
    if (!request.gm)
    {
        if (CampaignRules::NeedsHistory(false, ActivityDirector::Instance().CleanRuns(ActivityKindId::PugRaid),
                                        services.config.pugRaidCleanRunsGate))
        {
            why = "posting requires the configured number of actual completed clean raid runs";
            return false;
        }
        auto const p10 = services.census.P10ExactLevel60(target.team, services.nowUnix, 168);
        if (!services.census.HasHistory(services.nowUnix, 168) || !p10 || *p10 < 10)
        {
            why = "posting requires a full week of exact-level-60 census with p10 at least ten";
            return false;
        }
    }
    return true;
}

bool PugRaidKind::Transitioning(ActivityServices& services, Activity& activity, ActivityMember const& member)
{
    auto const* const state = dynamic_cast<PugRaidState const*>(activity.state.get());
    auto const attempt = state ? state->entryAttemptMs.find(member.guid) : std::map<ObjectGuid, uint64>::const_iterator{};
    if (!state || attempt == state->entryAttemptMs.end() || services.nowMs > attempt->second + 10000)
        return false;
    Player* const bot = ObjectAccessor::FindConnectedPlayer(member.guid);
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    return ai && ai->GetCoaEpoch() == member.epoch && bot->IsBeingTeleported();
}

bool PugRaidKind::Prepare(ActivityServices& services, Activity& activity, CampaignState& state)
{
    auto& raid = static_cast<PugRaidState&>(state);
    raid.reserveLimit = std::clamp(services.config.pugRaidReservePerPlayer, 1u, 5u);
    auto const result = services.ops.SetLoot(activity.groupGuid, activity.leader, uint8(MASTER_LOOT),
                                            uint8(ITEM_QUALITY_RARE), activity.leader);
    if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
        return false;
    if (result != ActivityOpResult::Done)
    {
        activity.Finish(ActivityOutcome::Stopped, "native master-loot setup failed");
        return false;
    }
    return true;
}

std::string PugRaidKind::Reserve(ActivityServices&, Activity& activity, Player* player, uint32 itemId)
{
    auto* const state = dynamic_cast<PugRaidState*>(activity.state.get());
    ActivityMember const* const member = player ? activity.FindMember(player->GetGUID()) : nullptr;
    if (!state || activity.finishing || activity.phase != ActivityPhase::Recruiting || !member || !member->In() ||
        member->isBot || !player->GetSession() || player->GetSession()->IsBot() ||
        player->GetGroup() != CampaignWorld::GroupOf(activity))
        return "Soft reserves are available to accepted real raid members during recruitment.";
    if (!itemId)
    {
        state->reserves.erase(player->GetGUID());
        return "Your soft reserves are cleared.";
    }
    ItemTemplate const* const item = sObjectMgr->GetItemTemplate(itemId);
    if (!item || item->Quality < ITEM_QUALITY_RARE || item->RequiredLevel > 60)
        return "Choose an existing rare-or-better item usable at level 60 or below.";
    auto& items = state->reserves[player->GetGUID()];
    if (items.count(itemId))
        return "You already reserved that item.";
    if (items.size() >= state->reserveLimit)
        return "Your reserve allowance is full; clear it before choosing another item.";
    items.insert(itemId);
    return Acore::StringFormat("Soft reserve recorded: item {}. Reserves close when recruitment ends.", itemId);
}

bool PugRaidKind::Allocate(ActivityServices& services, Activity& activity, PugRaidState& state)
{
    Player* const leader = activity.Leader() ? ActivityWorld::FindMember(*activity.Leader()) : nullptr;
    Group* const group = CampaignWorld::GroupOf(activity);
    if (!leader || !group || group->GetMasterLooterGuid() != activity.leader || group->GetLootMethod() != MASTER_LOOT ||
        leader->IsInCombat())
        return false;
    bool complete = true;
    state.unclaimedLoot = false;
    for (auto const& spawn : leader->GetMap()->GetCreatureBySpawnIdStore())
    {
        Creature* const corpse = spawn.second;
        // The actual objective can die beyond the master's current loot scan range after a knockback/retreat.
        // Inspect its loot and use the ordinary corpse approach below before claiming allocation is complete.
        if (!corpse || corpse->IsAlive() || corpse->GetLootRecipientGroup() != group ||
            (corpse->GetGUID() != state.engagedGuid && leader->GetExactDist(corpse) > 30))
            continue;
        for (std::size_t slot = 0; slot < corpse->loot.items.size() && slot < 256; ++slot)
        {
            LootItem const& item = corpse->loot.items[slot];
            ItemTemplate const* const proto = sObjectMgr->GetItemTemplate(item.itemid);
            if (item.is_looted || !proto || proto->Quality < ITEM_QUALITY_RARE)
                continue;
            bool humanReserved = false;
            for (auto const& reservation : state.reserves)
                humanReserved = humanReserved || reservation.second.count(item.itemid) != 0;
            auto const key = std::make_pair(corpse->GetGUID(), uint8(slot));
            ObjectGuid winner;
            auto const old = state.winners.find(key);
            if (old != state.winners.end())
                winner = old->second;
            else
            {
                uint32 bestRoll = 0;
                for (ActivityMember const& member : activity.members)
                {
                    auto const reserved = state.reserves.find(member.guid);
                    bool const hasReserve = reserved != state.reserves.end() && reserved->second.count(item.itemid);
                    if (!CampaignRules::ReserveEligible(humanReserved, member.isBot, hasReserve, member.In()))
                        continue;
                    Player* const player = ActivityWorld::FindMember(member);
                    if (!player || player->GetMap() != corpse->GetMap() || player->GetGroup() != group ||
                        !player->IsAtLootRewardDistance(corpse) || !item.AllowedForPlayer(player, corpse->GetGUID()))
                        continue;
                    ItemPosCountVec dest;
                    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item.itemid, item.count) != EQUIP_ERR_OK)
                        continue;
                    uint32 const roll = urand(1, 100);
                    if (roll > bestRoll)
                    {
                        bestRoll = roll;
                        winner = member.guid;
                    }
                }
                if (!winner.IsEmpty())
                {
                    state.winners[key] = winner;
                    services.ops.PostToChannel(activity.leader, ActivityChatTarget::Party,
                        Acore::StringFormat("Item {}: winning roll {}. {}", item.itemid, bestRoll,
                                            humanReserved ? "Soft reserve honored." : "Open raid roll."));
                }
            }
            if (winner.IsEmpty())
            {
                state.unclaimedLoot = true;
                continue;  // Never award a reserved item to a bot when its reserving player cannot receive it.
            }
            if (!leader->IsWithinDistInMap(corpse, INTERACTION_DISTANCE))
            {
                ActivityPoint point = state.spawn.point;
                point.x = corpse->GetPositionX();
                point.y = corpse->GetPositionY();
                point.z = corpse->GetPositionZ();
                activity.SetCenter(point, 2.0f);
                activity.SetPhase(ActivityPhase::Marching, services.nowMs);
                activity.SetTravel(ActivityTravel::FollowLeader);
                activity.SetObjective(ObjectGuid::Empty);
                return false;
            }
            auto const result = services.ops.GiveMasterLoot(activity.leader, corpse->GetGUID(), uint8(slot), winner);
            if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
                complete = false;
            else if (result != ActivityOpResult::Done)
            {
                state.winners.erase(key);
                complete = false;
            }
        }
    }
    return complete;
}

void PugRaidKind::Encounter(ActivityServices& services, Activity& activity, CampaignState& state)
{
    auto& raid = static_cast<PugRaidState&>(state);
    Player* const leader = activity.Leader() ? ActivityWorld::FindMember(*activity.Leader()) : nullptr;
    if (!leader)
        return;
    if (!CampaignWorld::Roles(activity, 1, 2))
    {
        activity.Finish(ActivityOutcome::TooFew, "raid requires an actual tank and two actual healers");
        return;
    }
    bool entering = false;
    if (leader->GetMapId() == raid.target.raidMap)
        raid.entered = true;
    else if (raid.entered)
    {
        activity.Finish(ActivityOutcome::Stopped, "raid leader left the raid instance");
        return;
    }
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot)
            if (Player* const bot = ActivityWorld::FindMember(member); bot && bot->IsAlive() &&
                bot->GetMapId() != raid.target.raidMap)
            {
                entering = true;
                if (bot->GetMapId() != raid.portal.mapId)
                {
                    activity.Finish(ActivityOutcome::Stopped, "raid bot left its ordinary portal route");
                    return;
                }
                auto const previous = raid.entryAttemptMs.find(member.guid);
                if (previous != raid.entryAttemptMs.end() && services.nowMs < previous->second + 3000)
                    continue;
                AreaTrigger const* const trigger = sObjectMgr->GetAreaTrigger(raid.portalTrigger);
                if (!trigger || !bot->IsInAreaTriggerRadius(trigger))
                    continue;
                auto const result = services.ops.EnterPortal(member.guid, raid.portalTrigger);
                if (result == ActivityOpResult::Deferred)
                    break;
                raid.entryAttemptMs[member.guid] = services.nowMs;
            }
    if (entering)
    {
        if (!raid.entryUntilMs)
            raid.entryUntilMs = services.nowMs + 300000;
        if (services.nowMs >= raid.entryUntilMs)
        {
            activity.Finish(ActivityOutcome::Stopped, "ordinary raid portal entry did not finish within five minutes");
            return;
        }
        activity.SetCenter(raid.portal, 1.0f);
        activity.SetPhase(ActivityPhase::Marching, services.nowMs);
        activity.SetTravel(ActivityTravel::None);
        return;  // Every bot enters through the actual native portal while physically inside its trigger.
    }
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot)
            if (Player* bot = ActivityWorld::FindMember(member); bot && bot->IsAlive() && bot->GetMap() != leader->GetMap())
            {
                activity.Finish(ActivityOutcome::Stopped, "raid bots entered different native instances");
                return;
            }
    activity.SetCenter(raid.spawn.point, 25.0f);
    activity.SetTravel(ActivityTravel::FollowLeader);
    CampaignWorld::Strategy(activity, raid);
    bool const lootDone = Allocate(services, activity, raid);
    Creature* const objective = CampaignWorld::SpawnFor(activity, raid);
    if (objective && !objective->IsAlive() && raid.engagedGuid == objective->GetGUID() &&
        objective->GetLootRecipientGroup() == CampaignWorld::GroupOf(activity))
    {
        activity.SetObjective(ObjectGuid::Empty);
        if (lootDone)
            activity.SetTravel(ActivityTravel::None);
        if (!raid.lootUntilMs)
            raid.lootUntilMs = services.nowMs + 60000;
        if (lootDone || services.nowMs >= raid.lootUntilMs)
            activity.Finish(ActivityOutcome::Success, lootDone && !raid.unclaimedLoot
                ? "raid objective killed; native loot allocation complete"
                : "raid objective killed; unallocated loot remains for players");
        return;
    }
    if (!lootDone && !leader->IsInCombat())
        return;
    CampaignKind::Encounter(services, activity, raid);
}

void PugRaidTactics::Update(PlayerbotAI* ai, uint64 nowMs)
{
    if (!ai || !ActivityRegistry::AnyHeld())
        return;
    ActivityBotState& state = CoaActivityBot::Refresh(ai);
    ActivityMemberView const* const row = state.row;
    Player* const bot = ai->GetBot();
    if (!row || row->kind != ActivityKindId::PugRaid || row->phase != ActivityPhase::Marching ||
        row->radius != 1.0f || !row->center.set || row->center.mapId != bot->GetMapId() ||
        !CampaignWorld::Safe(bot) || bot->IsInCombat() || nowMs < state.pugPortalMoveMs)
        return;
    state.pugPortalMoveMs = nowMs + 2000;
    if (bot->GetExactDist(row->center.x, row->center.y, row->center.z) > 0.5f)
        bot->GetMotionMaster()->MovePoint(0, row->center.x, row->center.y, row->center.z);
}

std::unique_ptr<ActivityKind> CreatePugRaidKind() { return std::make_unique<PugRaidKind>(); }
