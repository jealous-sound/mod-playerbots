/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CampaignSupport.h"
#include "ActivityOperations.h"
#include "ActivityPvePolicy.h"
#include "CoaThread.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFGMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

bool CampaignWorld::Safe(Player* player, bool allowInstance, uint32 pvpRuleset)
{
    return player && player->IsInWorld() && !player->IsDuringRemoveFromWorld() && player->GetSession() &&
           !player->GetSession()->isLogingOut() && player->GetMap() &&
           (allowInstance || !player->GetMap()->Instanceable()) && player->IsAlive() && !player->GetVehicle() &&
           !player->IsBeingTeleported() && !player->IsInFlight() && !player->duel &&
           (!player->IsPvP() || pvpRuleset == ActivityThreshold::SPELL_WAR_MODE) &&
           !player->IsFFAPvP() && (!player->HasAura(ActivityThreshold::SPELL_WAR_MODE) ||
                                 pvpRuleset == ActivityThreshold::SPELL_WAR_MODE) &&
           !player->HasAura(ActivityThreshold::SPELL_HIGH_RISK) &&
           !player->HasAura(ActivityThreshold::SPELL_MERCENARY) && !player->InBattleground() &&
           !player->InArena() && !player->InBattlegroundQueue() &&
           sLFGMgr->GetState(player->GetGUID()) == lfg::LFG_STATE_NONE;
}

Group* CampaignWorld::GroupOf(Activity const& activity)
{
    return activity.groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
}

CampaignState* CampaignWorld::State(Activity& activity) { return dynamic_cast<CampaignState*>(activity.state.get()); }

bool CampaignWorld::In(Activity const& activity, ObjectGuid guid)
{
    ActivityMember const* const member = activity.FindMember(guid);
    return member && member->In();
}

bool CampaignWorld::GroupEngaged(Activity const& activity, Creature* creature)
{
    Unit* const victim = creature ? creature->GetVictim() : nullptr;
    Player* const owner = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    return creature && creature->IsInCombat() && owner && In(activity, owner->GetGUID());
}

bool CampaignWorld::OutsiderEngaged(Activity const& activity, Creature* creature)
{
    if (!creature)
        return false;
    Player* const looter = creature->GetLootRecipient();
    if (looter && !In(activity, looter->GetGUID()) && creature->GetLootRecipientGroup() != GroupOf(activity))
        return true;
    Unit* const victim = creature->GetVictim();
    Player* const owner = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    return creature->IsInCombat() && owner && !In(activity, owner->GetGUID());
}

Creature* CampaignWorld::SpawnFor(Activity const& activity, CampaignState const& state)
{
    if (!state.settings.instance)
        return ActivityGuards::FindSpawn(state.spawn.spawnId);
    ActivityMember const* const member = activity.FindMember(activity.leader);
    Player* const leader = member ? ActivityWorld::FindMember(*member) : nullptr;
    if (!leader || !leader->GetMap() || leader->GetMapId() != state.target.raidMap)
        return nullptr;
    auto const range = leader->GetMap()->GetCreatureBySpawnIdStore().equal_range(state.spawn.spawnId);
    Creature* dead = nullptr;
    for (auto it = range.first; it != range.second; ++it)
        if (Creature* const creature = it->second; creature && creature->IsInWorld())
        {
            if (creature->IsAlive())
                return creature;
            dead = creature;
        }
    return dead;
}

bool CampaignWorld::Roles(Activity const& activity, uint32 tanks, uint32 heals)
{
    for (ActivityMember const& member : activity.members)
        if (member.In())
            if (Player* const player = ActivityWorld::FindMember(member); player && player->IsAlive())
            {
                if (PlayerbotAI::IsTank(player) && tanks)
                    --tanks;
                else if (PlayerbotAI::IsHeal(player) && heals)
                    --heals;
            }
    return !tanks && !heals;
}

bool CampaignWorld::GuardLevels(ActivityCampaignTarget const& target, std::string& why, float yards,
                               bool completeCensus)
{
    CreatureData const* const hub = sObjectMgr->GetCreatureData(target.spawnId);
    if (!hub || target.guardSpawns.empty() || !std::isfinite(yards) || yards <= 0)
    {
        why = "guard measurements require an explicit nonempty spawn list";
        return false;
    }
    for (uint32 const id : target.guardSpawns)
    {
        CreatureData const* const data = sObjectMgr->GetCreatureData(id);
        CreatureTemplate const* const ct = data ? sObjectMgr->GetCreatureTemplate(data->id) : nullptr;
        if (!ct || data->id2 || data->id3 || ct->maxlevel > 60 || ct->minlevel > 60 ||
            data->mapid != target.rally.mapId || std::hypot(data->posX - hub->posX, data->posY - hub->posY) > yards)
        {
            why = Acore::StringFormat("guard spawn {} is unknown, on another map, or above level 60", id);
            return false;
        }
        if (Creature* const guard = ActivityGuards::FindSpawn(id); guard && guard->GetLevel() > 60)
        {
            why = Acore::StringFormat("loaded guard {} exceeds level 60", id);
            return false;
        }
    }
    if (completeCensus)
        for (auto const& entry : sObjectMgr->GetAllCreatureData())
        {
            CreatureData const& data = entry.second;
            if (data.mapid != hub->mapid || std::hypot(data.posX - hub->posX, data.posY - hub->posY) > yards)
                continue;
            bool isGuard = false;
            for (uint32 id : {data.id, data.id2, data.id3})
                if (CreatureTemplate const* ct = id ? sObjectMgr->GetCreatureTemplate(id) : nullptr)
                    isGuard = isGuard || (ct->flags_extra & CREATURE_FLAG_EXTRA_GUARD) != 0;
            if (isGuard && std::find(target.guardSpawns.begin(), target.guardSpawns.end(), entry.first) ==
                               target.guardSpawns.end())
            {
                why = Acore::StringFormat("nearby native guard spawn {} is missing from the measurements", entry.first);
                return false;
            }
        }
    if (Creature* const actual = ActivityGuards::FindSpawn(target.spawnId))
        for (auto const& entry : actual->GetMap()->GetCreatureBySpawnIdStore())
            if (Creature* const guard = entry.second; guard && guard->IsAlive() && guard->GetLevel() > 60 &&
                guard->IsGuard() && guard->GetExactDist(actual) <= yards)
            {
                why = "a currently loaded guard in the measured hub radius exceeds level 60";
                return false;
            }
    return true;
}

bool CampaignWorld::ResolveTarget(ActivityCampaignTarget const& target, ActivitySpawnInfo& spawn,
                                  std::string& why, bool allowInstance)
{
    CreatureData const* const data = target.spawnId ? sObjectMgr->GetCreatureData(target.spawnId) : nullptr;
    CreatureTemplate const* const ct = data ? sObjectMgr->GetCreatureTemplate(data->id) : nullptr;
    MapEntry const* const map = data ? sMapStore.LookupEntry(data->mapid) : nullptr;
    if (!data || data->id2 || data->id3 || !ct || !map || (!allowInstance && map->Instanceable()) || ct->maxlevel > 63 ||
        !Acore::IsValidMapCoord(data->posX, data->posY, data->posZ))
    {
        why = "target spawn is unknown, has an invalid map, or exceeds level 63";
        return false;
    }
    spawn.spawnId = target.spawnId;
    spawn.entry = data->id;
    spawn.point.mapId = data->mapid;
    spawn.point.x = data->posX;
    spawn.point.y = data->posY;
    spawn.point.z = data->posZ;
    spawn.point.set = true;
    spawn.level = ct->maxlevel;
    spawn.npcFlags = ct->npcflag;
    spawn.respawnSeconds = data->spawntimesecs;
    return true;
}

void CampaignWorld::Strategy(Activity& activity, CampaignState& state)
{
    if (state.target.strategy.empty() || !state.settings.instance)
        return;
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot)
            if (Player* const bot = ActivityWorld::FindMember(member))
            {
                PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);
                for (BotState const engine : {BOT_STATE_NON_COMBAT, BOT_STATE_COMBAT})
                    if (!ai->HasStrategy(state.target.strategy, engine))
                    {
                        ai->ChangeStrategy("+" + state.target.strategy, engine);
                        state.addedStrategy[member.guid] |= engine == BOT_STATE_COMBAT ? 2 : 1;
                        state.strategyEpoch[member.guid] = member.epoch;
                    }
            }
}

void CampaignWorld::RestoreStrategies(Activity& activity, CampaignState& state)
{
    for (auto const& entry : state.addedStrategy)
    {
        Player* const bot = ObjectAccessor::FindConnectedPlayer(entry.first);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!ai || ai->GetCoaEpoch() != state.strategyEpoch[entry.first])
            continue;
        if (entry.second & 1)
            ai->ChangeStrategy("-" + state.target.strategy, BOT_STATE_NON_COMBAT);
        if (entry.second & 2)
            ai->ChangeStrategy("-" + state.target.strategy, BOT_STATE_COMBAT);
    }
    state.addedStrategy.clear();
    state.strategyEpoch.clear();
    (void)activity;
}

std::optional<std::vector<std::string>> CampaignKind::Targets(ActivityServices& services,
                                                            ActivityStartRequest const& request)
{
    std::vector<std::string> names;
    for (ActivityCampaignTarget const& target : Configured(services.config))
    {
        auto state = NewState();
        state->settings = Settings(services.config);
        std::string why;
        uint8 const team = state->settings.defense ? uint8(1 - target.team) : target.team;
        if ((!request.team || *request.team == team) && Validate(services, request, target, *state, why))
            names.push_back(target.name);
        if (names.size() == 4)
            break;
    }
    return names;
}

bool CampaignKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                        std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    CampaignSettings const settings = Settings(services.config);
    if (!settings.enabled)
    {
        why = "campaign disabled";
        return false;
    }
    auto state = NewState();
    state->settings = settings;
    ActivityCampaignTarget const* chosen = nullptr;
    for (ActivityCampaignTarget const& target : Configured(services.config))
    {
        uint8 const team = settings.defense ? uint8(1 - target.team) : target.team;
        if (target.team > 1 || (!request.target.empty() && request.target != target.name) ||
            (request.team && *request.team != team) ||
            (!request.team && request.nearPlayer && request.nearPlayer->GetTeamId() != team))
            continue;
        if (!Validate(services, request, target, *state, why))
            continue;
        auto const prior = lastStarted.find(target.spawnId);
        if (prior != lastStarted.end() && services.nowMs < prior->second + uint64(settings.cooldownMinutes) * 60000)
        {
            why = "originating target is on cooldown";
            continue;
        }
        chosen = &target;
        break;
    }
    if (!chosen)
    {
        if (why.empty())
            why = "no verified campaign target fits";
        return false;
    }
    state->target = *chosen;
    state->rally = chosen->rally;
    MapEntry const* const rallyMap = sMapStore.LookupEntry(state->rally.mapId);
    if (!state->rally.set || !rallyMap || rallyMap->Instanceable() ||
        !Acore::IsValidMapCoord(state->rally.x, state->rally.y, state->rally.z))
    {
        why = "campaign requires a valid explicit overworld rally";
        return false;
    }
    ActivityPoint const ground = ActivityWorld::GroundNear(state->rally, 0);
    if (!ground.set || std::abs(ground.z - state->rally.z) > 20.0f)
    {
        why = "rally is not on dry ground";
        return false;
    }
    state->rally = ground;
    uint32 const wanted = std::min(request.capacity,
        std::clamp(request.size ? request.size : settings.maximum, settings.minimum, settings.maximum));
    if (wanted < settings.minimum)
    {
        why = "not enough participant capacity";
        return false;
    }
    ActivityCandidateQuery query;
    query.team = settings.defense ? uint8(1 - chosen->team) : chosen->team;
    query.minLevel = state->settings.lowest;
    query.maxLevel = state->settings.highest;
    query.center = state->rally;
    query.sameMapOnly = true;
    query.maxDistance = 2000.0f;
    query.rulesetSafe = true;
    query.limit = 2 * wanted;
    auto candidates = services.candidates.Find(query);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](auto const& candidate)
    {
        Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!ai || ai->GetCoaEpoch() != candidate.epoch || !CampaignWorld::Safe(bot))
            return true;
        if (settings.defense || settings.instance || settings.anchor)
            return false;
        Creature* const target = ActivityGuards::FindSpawn(state->spawn.spawnId);
        return !target || !target->IsAlive() || target->GetMap() != bot->GetMap() ||
               !bot->IsValidAttackTarget(target);
    }), candidates.end());
    if (candidates.size() < settings.minimum)
    {
        why = "too few nearby safe compatible bots, or PvE target cannot actually be attacked";
        return false;
    }
    // A real tank leads where one exists. This ordering is stable and does not consume RNG.
    auto const tank = std::find_if(candidates.begin(), candidates.end(), [](auto const& candidate)
    {
        return PlayerbotAI::IsTank(ObjectAccessor::FindConnectedPlayer(candidate.guid));
    });
    if (tank != candidates.end())
        std::rotate(candidates.begin(), tank, tank + 1);
    plan.team = query.team;
    plan.minLevel = state->settings.lowest;
    plan.maxLevel = state->settings.highest;
    plan.rally = state->rally;
    plan.name = chosen->name;
    plan.target = chosen->name;
    plan.wanted = wanted;
    plan.needed = settings.minimum;
    plan.leaderFixed = true;
    plan.candidates = std::move(candidates);
    plan.state = std::move(state);
    return true;
}

bool CampaignKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    CampaignState* const state = dynamic_cast<CampaignState*>(plan.state.get());
    if (!state || !Settings(services.config).enabled || plan.decisions.empty() || plan.decisions[0] != 1)
        return false;
    ActivityStartRequest request;
    request.gm = plan.gm;
    request.team = plan.team;
    request.target = plan.target;
    std::string why;
    if (!Validate(services, request, state->target, *state, why))
        return false;
    std::vector<ActivityCandidate const*> chosen;
    for (std::size_t i = 0; i < plan.candidates.size() && chosen.size() < plan.wanted; ++i)
    {
        auto const& candidate = plan.candidates[i];
        Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        bool const accepted = i < plan.decisions.size() && plan.decisions[i] == 1;
        bool const fits = accepted && ai && ai->GetCoaEpoch() == candidate.epoch &&
            ActivityCandidates::IsFree(bot) && !bot->IsInCombat() && CampaignWorld::Safe(bot) &&
            bot->GetTeamId() == plan.team && bot->GetLevel() >= plan.minLevel && bot->GetLevel() <= plan.maxLevel &&
            bot->GetMapId() == state->rally.mapId;
        if (!i && !fits)
            return false;
        if (fits)
            chosen.push_back(&candidate);
    }
    if (chosen.size() < plan.needed)
        return false;
    activity.leader = chosen.front()->guid;
    activity.name = plan.name;
    activity.target = plan.target;
    activity.minLevel = plan.minLevel;
    activity.maxLevel = plan.maxLevel;
    activity.pvpRuleset = state->settings.pvpRuleset;
    activity.warMode = activity.pvpRuleset == ActivityThreshold::SPELL_WAR_MODE;
    for (auto const* candidate : chosen)
        activity.Enroll(candidate->guid, candidate->epoch, true, candidate->level, services.nowUnix);
    state->formUntilMs = services.nowMs + ActivityThreshold::FORM_TIMEOUT_MS;
    state->timeoutMs = services.nowMs + uint64(state->settings.minutes) * 60000;
    activity.hardDeadlineMs = state->timeoutMs + 60000;
    activity.SetCenter(state->rally, 25.0f);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    activity.SetPhase(ActivityPhase::Forming, services.nowMs);
    lastStarted[state->spawn.spawnId] = services.nowMs;
    activity.state = std::move(plan.state);
    return true;
}

void CampaignKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    CampaignState* const state = CampaignWorld::State(activity);
    if (!state || activity.finishing)
        return;
    if (!Settings(services.config).enabled)
    {
        activity.Finish(ActivityOutcome::Disabled, "campaign disabled on reload");
        return;
    }
    if (Settings(services.config).pvpRuleset != activity.pvpRuleset)
    {
        activity.Finish(ActivityOutcome::Disabled, "campaign ruleset policy changed on reload");
        return;
    }
    if (services.nowMs >= state->timeoutMs)
    {
        activity.Finish(ActivityOutcome::Timeout, "campaign duration expired");
        return;
    }
    uint32 alive = 0;
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot)
        {
            if (Transitioning(services, activity, member))
                return;
            Player* const bot = ActivityWorld::FindMember(member);
            if (bot && !bot->IsAlive())
                continue;  // Ordinary corpse runs only; a casualty does not grant a director resurrection.
            if (!bot || !CampaignWorld::Safe(bot, state->settings.instance, activity.pvpRuleset) ||
                bot->GetTeamId() != activity.team ||
                bot->GetLevel() < activity.minLevel || bot->GetLevel() > activity.maxLevel ||
                (activity.pvpRuleset && member.rulesetApplied && (!bot->HasAura(activity.pvpRuleset) ||
                    bot->HasAura(ActivityThreshold::SPELL_PVE))))
            {
                activity.Finish(ActivityOutcome::Stopped, "a campaign bot became unavailable or unsafe");
                return;
            }
            ++alive;
        }
    if (alive < state->settings.minimum)
    {
        activity.Finish(ActivityOutcome::TooFew, "too few campaign bots remain");
        return;
    }
    Group* group = CampaignWorld::GroupOf(activity);
    if (activity.phase == ActivityPhase::Forming)
    {
        if (services.nowMs >= state->formUntilMs)
        {
            activity.Finish(ActivityOutcome::TooFew, "group formation deadline expired");
            return;
        }
        if (!group)
        {
            std::vector<ObjectGuid> members;
            for (ActivityMember const& member : activity.members)
                if (member.In() && member.guid != activity.leader)
                    members.push_back(member.guid);
            std::vector<ObjectGuid> added;
            std::vector<ObjectGuid> skipped;
            ObjectGuid formed;
            auto const result = services.ops.FormGroup(activity.leader, members,
                uint8(state->settings.instance ? MASTER_LOOT : GROUP_LOOT), uint8(ITEM_QUALITY_RARE),
                formed, added, skipped);
            if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
                return;
            if (result != ActivityOpResult::Done)
            {
                activity.Finish(ActivityOutcome::TooFew, "group formation failed");
                return;
            }
            activity.groupGuid = formed;
            activity.viewDirty = true;
            state->toAdd = std::move(skipped);
            group = CampaignWorld::GroupOf(activity);
        }
        for (auto it = state->toAdd.begin(); it != state->toAdd.end();)
        {
            auto const result = services.ops.AddMember(activity.groupGuid, activity.leader, *it);
            if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
                return;
            if (result != ActivityOpResult::Done)
            {
                activity.Finish(ActivityOutcome::TooFew, "registered member could not join campaign group");
                return;
            }
            it = state->toAdd.erase(it);
        }
        if (!group || group->GetMembersCount() < state->settings.minimum)
        {
            activity.Finish(ActivityOutcome::TooFew, "campaign group is smaller than its registered members");
            return;
        }
        if (state->settings.raid && services.ops.ConvertToRaid(activity.groupGuid, activity.leader) != ActivityOpResult::Done)
            return;
        if (!Prepare(services, activity, *state))
            return;
        state->recruitUntilMs = services.nowMs + uint64(state->settings.recruitSeconds) * 1000;
        activity.SetFlags(ACTIVITY_VIEW_QUIET | ACTIVITY_VIEW_RECRUITING);
        activity.SetPhase(ActivityPhase::Recruiting, services.nowMs);
    }
    CampaignWorld::Strategy(activity, *state);
    if (activity.phase == ActivityPhase::Recruiting)
    {
        if (activity.CanPost(services.config, services.nowMs) && services.nowMs >= state->nextPostMs)
        {
            std::string text = Acore::StringFormat("LFM {}: {}/{}; {}. Whisper me to join.", activity.name,
                activity.BotsIn() + activity.PlayersIn(), state->settings.raid ? 40 : 5,
                state->settings.instance ? "soft reserves; master loot; reserves close before entry"
                    : activity.pvpRuleset ? "War Mode; registered bot defenders" : "PvE only");
            auto const result = services.ops.PostToChannels(activity.leader, services.config.chatChannels, text,
                state->rally.zoneId, ActivityWorld::NearestAudience(services.realPlayers, activity.team, state->rally));
            if (result == ActivityOpResult::Done)
                activity.NotePost(services.nowMs);
            state->nextPostMs = services.nowMs + uint64(services.config.chatRecruitEverySeconds) * 1000;
        }
        if (services.nowMs < state->recruitUntilMs)
            return;
        state->gatherUntilMs = services.nowMs + uint64(services.config.travelGatherTimeoutSeconds) * 1000;
        activity.SetFlags(0);
        activity.SetTravel(ActivityTravel::Gather);
        activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
    }
    if (activity.phase == ActivityPhase::Gathering)
    {
        uint32 gathered = 0;
        for (ActivityMember const& member : activity.members)
            if (member.In() && member.isBot)
                if (Player* const bot = ActivityWorld::FindMember(member); bot && bot->IsAlive() &&
                    bot->GetMapId() == state->rally.mapId &&
                    bot->GetExactDist(state->rally.x, state->rally.y, state->rally.z) <= activity.radius)
                    ++gathered;
        if (gathered < activity.BotsIn())
        {
            if (services.nowMs >= state->gatherUntilMs)
                activity.Finish(ActivityOutcome::TooFew, "campaign rally deadline expired");
            return;
        }
        activity.SetFlags(0);
        activity.SetPhase(ActivityPhase::Marching, services.nowMs);
        activity.SetTravel(state->settings.instance ? ActivityTravel::None : ActivityTravel::FollowLeader);
        activity.SetCenter(state->settings.instance ? state->rally : state->spawn.point, 30.0f);
    }
    if (activity.phase == ActivityPhase::Marching || activity.phase == ActivityPhase::Active)
        Encounter(services, activity, *state);
}

void CampaignKind::Encounter(ActivityServices& services, Activity& activity, CampaignState& state)
{
    Creature* const creature = CampaignWorld::SpawnFor(activity, state);
    if (!creature)
    {
        if (state.engaged)
            activity.Finish(ActivityOutcome::Stopped, "engaged target unloaded; no kill inferred");
        return;
    }
    if (CampaignWorld::OutsiderEngaged(activity, creature))
    {
        activity.SetObjective(ObjectGuid::Empty);
        activity.Finish(ActivityOutcome::Stopped, "target belongs to an outside group");
        return;
    }
    if (!creature->IsAlive())
    {
        bool const credited = state.engagedGuid == creature->GetGUID() &&
            creature->GetLootRecipientGroup() == CampaignWorld::GroupOf(activity);
        activity.Finish(credited ? ActivityOutcome::Success : ActivityOutcome::Stopped,
                        credited ? "registered group killed its objective" : "target died without group attribution");
        return;
    }
    Player* const leader = activity.Leader() ? ActivityWorld::FindMember(*activity.Leader()) : nullptr;
    if (!leader || leader->GetMap() != creature->GetMap() || !leader->InSamePhase(creature))
        return;
    std::unordered_set<uint32> participants;
    for (ActivityMember const& member : activity.members)
        if (member.In() && !member.isBot)
            participants.insert(member.guid.GetCounter());
    if (!leader->IsValidAttackTarget(creature) || ActivityGuards::QuestTargetNeeded(state.spawn.entry,
        services.realPlayers, &participants))
    {
        activity.SetObjective(ObjectGuid::Empty);
        activity.Finish(ActivityOutcome::Stopped, "objective became invalid or required by a player's quest");
        return;
    }
    if (creature->IsInEvadeMode() && state.engaged)
    {
        activity.Finish(ActivityOutcome::Stopped, "single attempt ended by objective evade");
        return;
    }
    if (leader->GetExactDist(creature) > ActivityThreshold::OBJECTIVE_REACH_YARDS &&
        !CampaignWorld::GroupEngaged(activity, creature))
        return;
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
    activity.SetObjective(creature->GetGUID());
    state.engagedGuid = creature->GetGUID();
    if (CampaignWorld::GroupEngaged(activity, creature))
    {
        state.engaged = true;
        state.engagedGuid = creature->GetGUID();
        if (!state.engagedSinceMs)
            state.engagedSinceMs = services.nowMs;
    }
}

bool CampaignKind::Accepts(ActivityServices&, Activity const& activity, Player* player)
{
    auto const* const state = dynamic_cast<CampaignState const*>(activity.state.get());
    Group* const group = CampaignWorld::GroupOf(activity);
    return state && activity.phase == ActivityPhase::Recruiting && !activity.finishing &&
           CampaignWorld::Safe(player, false, activity.pvpRuleset) && !player->GetSession()->IsBot() && !player->GetGroup() &&
           !player->GetGroupInvite() && player->GetTeamId() == activity.team &&
           (!activity.pvpRuleset || (player->HasAura(activity.pvpRuleset) && player->IsPvP() &&
                                    !player->HasAura(ActivityThreshold::SPELL_PVE))) &&
           player->GetLevel() >= activity.minLevel && player->GetLevel() <= activity.maxLevel && group &&
           group->GetMembersCount() < (state->settings.raid ? 40 : 5);
}

std::string CampaignKind::Join(ActivityServices& services, Activity& activity, Player* player)
{
    if (!Accepts(services, activity, player))
        return "This campaign cannot invite you now.";
    auto const result = services.ops.InvitePlayer(activity.leader, player->GetGUID());
    if (result == ActivityOpResult::Done)
    {
        activity.invites[player->GetGUID()] = services.nowMs + ActivityThreshold::INVITE_TIMEOUT_MS;
        return "Invitation sent. Accept it to join the campaign.";
    }
    return result == ActivityOpResult::Deferred ? "The invitation budget is busy; please try again shortly."
                                                : "The group could not send an invitation now.";
}

void CampaignKind::OnMemberLeft(ActivityServices&, Activity& activity, ActivityMember const& member)
{
    if (CampaignState* const state = CampaignWorld::State(activity))
    {
        auto const added = state->addedStrategy.find(member.guid);
        auto const epoch = state->strategyEpoch.find(member.guid);
        if (added != state->addedStrategy.end() && epoch != state->strategyEpoch.end())
        {
            Player* const bot = ObjectAccessor::FindConnectedPlayer(member.guid);
            PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
            if (ai && ai->GetCoaEpoch() == epoch->second)
            {
                if (added->second & 1)
                    ai->ChangeStrategy("-" + state->target.strategy, BOT_STATE_NON_COMBAT);
                if (added->second & 2)
                    ai->ChangeStrategy("-" + state->target.strategy, BOT_STATE_COMBAT);
            }
            state->addedStrategy.erase(added);
            state->strategyEpoch.erase(epoch);
        }
    }
    if (member.guid == activity.leader)
        activity.Finish(ActivityOutcome::Stopped, "campaign leader left");
}

void CampaignKind::OnEnd(ActivityServices& services, Activity& activity)
{
    if (CampaignState* const state = CampaignWorld::State(activity))
    {
        Cleanup(services, activity, *state);
        CampaignWorld::RestoreStrategies(activity, *state);
    }
    activity.SetObjective(ObjectGuid::Empty);
}

std::string CampaignKind::Describe(Activity const& activity, uint64 nowMs) const
{
    auto const* const state = dynamic_cast<CampaignState const*>(activity.state.get());
    return state ? Acore::StringFormat("campaign spawn {} entry {} strategy {} engaged {}; {} s remain",
        state->spawn.spawnId, state->spawn.entry, state->target.strategy, state->engaged ? "yes" : "no",
        (state->timeoutMs - std::min(state->timeoutMs, nowMs)) / 1000) : "campaign state unavailable";
}
