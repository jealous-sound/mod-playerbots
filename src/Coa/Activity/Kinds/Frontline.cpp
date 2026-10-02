/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#include "Frontline.h"
#include "ActivityCensus.h"
#include "ActivityDirector.h"
#include "ActivityPvpPolicy.h"
#include "ActivityRegistry.h"
#include "Creature.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include <algorithm>

CampaignSettings FrontlineKind::Settings(ActivityConfig const& config) const
{
    CampaignSettings s;
    s.enabled = defense ? config.hubDefenseEnable : config.frontlineEnable;
    s.minimum = config.frontlineMinBots;
    s.maximum = config.frontlineMaxBots;
    s.recruitSeconds = std::max(config.frontlineRecruitSeconds, config.frontlineWarningSeconds);
    s.minutes = config.frontlineMinutes;
    s.cooldownMinutes = config.frontlineCooldownMinutes;
    s.raid = s.maximum > 5;
    s.defense = defense;
    if (config.frontlinePvpDefendersEnable)
        s.pvpRuleset = ActivityThreshold::SPELL_WAR_MODE;
    return s;
}

std::vector<ActivityCampaignTarget> const& FrontlineKind::Configured(ActivityConfig const& config) const
{
    return config.frontlineTargets;
}

bool FrontlineKind::Validate(ActivityServices& services, ActivityStartRequest const& request,
                             ActivityCampaignTarget const& target, CampaignState& state, std::string& why) const
{
    if (!CampaignWorld::ResolveTarget(target, state.spawn, why) ||
        !CampaignWorld::GuardLevels(target, why, float(services.config.frontlineGuardRadius)))
        return false;
    if (state.spawn.level > 60 || target.rally.mapId != state.spawn.point.mapId)
    {
        why = "hub objective exceeds level 60 or is not on the rally map";
        return false;
    }
    uint32 highest = state.spawn.level;
    for (uint32 guard : target.guardSpawns)
        if (CreatureData const* data = sObjectMgr->GetCreatureData(guard))
            if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data->id))
                highest = std::max(highest, uint32(ct->maxlevel));
    state.settings.lowest = uint8(std::max(1u, highest > 5 ? highest - 5 : 1u));
    state.settings.highest = uint8(std::min(60u, highest + 5));
    Creature* const hub = ActivityGuards::FindSpawn(target.spawnId);
    if (!hub || !hub->IsAlive() || hub->IsDuringRemoveFromWorld())
    {
        why = "hub objective must be loaded and alive";
        return false;
    }
    if (!defense && ActivityGuards::QuestGiverHeld(state.spawn.entry, services.realPlayers))
    {
        why = "a real player holds a quest from this hub objective";
        return false;
    }
    if (!request.gm && !defense)
    {
        if (!services.config.frontlineAttackVerified || CampaignRules::NeedsHistory(false,
            ActivityDirector::Instance().CleanRuns(ActivityKindId::Raid), services.config.frontlineCleanRunsGate))
        {
            why = "frontline requires an actual verified PvE attack and completed clean outpost cycles";
            return false;
        }
        for (uint8 team : {uint8(0), uint8(1)})
        {
            auto const p10 = services.census.RaidPoolP10(team, services.nowUnix);
            if (!p10 || *p10 < 2 * services.config.raidMinBots)
            {
                why = "both factions need an outpost raid census p10 of twice Raid.MinBots";
                return false;
            }
        }
    }
    return true;
}

void FrontlineKind::Encounter(ActivityServices& services, Activity& activity, CampaignState& state)
{
    std::string why;
    if (!CampaignWorld::GuardLevels(state.target, why, float(services.config.frontlineGuardRadius), false))
    {
        activity.Finish(ActivityOutcome::Stopped, "hub guard measurements changed: " + why);
        return;
    }
    Creature* const hub = ActivityGuards::FindSpawn(state.spawn.spawnId);
    if (!hub || !hub->IsAlive())
    {
        if (defense)
            activity.Finish(ActivityOutcome::Stopped, "defended hub became unavailable");
        else
            CampaignKind::Encounter(services, activity, state);
        return;
    }
    if (!defense)
    {
        uint8 const defenders = uint8(1 - activity.team);
        if (ActivityGuards::QuestGiverHeld(state.spawn.entry, services.realPlayers) ||
            ActivityGuards::TeamNear(services.realPlayers, defenders, state.spawn.point,
                                    float(services.config.frontlineGuardRadius)))
        {
            activity.SetObjective(ObjectGuid::Empty);
            activity.Finish(ActivityOutcome::Stopped, "real hub defender or quest holder is protected");
            return;
        }
        CampaignKind::Encounter(services, activity, state);
        return;
    }
    Player* const leader = activity.Leader() ? ActivityWorld::FindMember(*activity.Leader()) : nullptr;
    if (!leader || leader->GetMap() != hub->GetMap() || !leader->IsFriendlyTo(hub))
    {
        activity.Finish(ActivityOutcome::Stopped, "defender group is not friendly to its originating hub");
        return;
    }
    activity.SetCenter(state.spawn.point, 25.0f);
    activity.SetTravel(ActivityTravel::FollowLeader);
    if (leader->GetExactDist(hub) > 25.0f)
    {
        activity.SetPhase(ActivityPhase::Marching, services.nowMs);
        activity.SetFlags(0);
        return;
    }
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
    Unit* threat = nullptr;
    for (Unit* const attacker : hub->getAttackers())
    {
        Player* const owner = attacker ? attacker->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (owner && activity.pvpRuleset)
        {
            PlayerbotAI* const ai = GET_PLAYERBOT_AI(owner);
            auto const row = ai ? ActivityRegistry::Find(owner->GetGUID().GetCounter()) : std::nullopt;
            if (row && row->epoch == ai->GetCoaEpoch() && row->kind == ActivityKindId::Frontline &&
                row->pvpRuleset == activity.pvpRuleset && owner->GetExactDist(hub) <= services.config.frontlineGuardRadius &&
                ActivityPvpPolicy::Eligible(leader, owner, activity.pvpRuleset) && leader->IsValidAttackTarget(attacker))
            {
                threat = attacker;
                break;
            }
        }
        if (Creature* const npc = attacker ? attacker->ToCreature() : nullptr;
            npc && npc->IsAlive() && npc->GetMap() == hub->GetMap() && !npc->GetCharmerOrOwnerPlayerOrPlayerItself() &&
            npc->GetLevel() <= 60 && npc->GetExactDist(hub) <= services.config.frontlineGuardRadius &&
            leader->IsValidAttackTarget(npc))
        {
            threat = npc;
            break;
        }
    }
    // Bot attackers are eligible only under the explicitly configured, registry-checked native PvP policy.
    activity.SetObjective(threat ? threat->GetGUID() : ObjectGuid::Empty);
    activity.SetFlags(threat ? 0 : ACTIVITY_VIEW_QUIET);
}

std::unique_ptr<ActivityKind> CreateFrontlineKind() { return std::make_unique<FrontlineKind>(); }
std::unique_ptr<ActivityKind> CreateHubDefenseKind() { return std::make_unique<FrontlineKind>(true); }
