/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#include "LeaderEncounter.h"
#include "ActivityGuards.h"
#include "ActivityOperations.h"
#include "ActivityPvePolicy.h"
#include "CoaThread.h"
#include "Creature.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LocalLevelScaling.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace
{
constexpr float RALLY_DISTANCE = 140.0f;
constexpr float RALLY_RADIUS = 20.0f;
constexpr float ISOLATION_RADIUS = 200.0f;
constexpr std::size_t MAX_GUARDS = 40;

LeaderEncounterState* StateOf(Activity& activity)
{
    return dynamic_cast<LeaderEncounterState*>(activity.state.get());
}

uint32 Minimum(ActivityConfig const& config, ActivityKindId kind)
{
    return kind == ActivityKindId::CapitalRaid ? config.capitalMinBots :
        kind == ActivityKindId::LieutenantRaid ? config.lieutenantMinBots : config.leaderMinBots;
}

uint32 Maximum(ActivityConfig const& config, ActivityKindId kind)
{
    return kind == ActivityKindId::CapitalRaid ? config.capitalMaxBots :
        kind == ActivityKindId::LieutenantRaid ? config.lieutenantMaxBots : config.leaderMaxBots;
}

std::vector<ActivityCampaignTarget> const& ConfiguredTargets(ActivityConfig const& config, ActivityKindId kind)
{
    return kind == ActivityKindId::CapitalRaid ? config.capitalTargets :
        kind == ActivityKindId::LieutenantRaid ? config.lieutenantTargets : config.leaderTargets;
}

ActivityCampaignTarget const* Configured(ActivityConfig const& config, ActivityKindId kind, std::string const& name)
{
    auto const& targets = ConfiguredTargets(config, kind);
    auto const found = std::find_if(targets.begin(), targets.end(), [&](auto const& target) { return target.name == name; });
    return found == targets.end() ? nullptr : &*found;
}

bool Enabled(ActivityConfig const& config, ActivityKindId kind)
{
    bool const enabled = kind == ActivityKindId::LeaderEncounter ? config.leaderEnable && config.leaderMax :
        kind == ActivityKindId::CapitalRaid ? config.capitalEnable && config.capitalMax :
        kind == ActivityKindId::LieutenantRaid ? config.lieutenantEnable && config.lieutenantMax : false;
    return enabled && config.leaderSideEffectsAccepted &&
        Minimum(config, kind) >= 1 && Minimum(config, kind) <= Maximum(config, kind) && Maximum(config, kind) <= 40 &&
        config.leaderMinLevel >= 1 && config.leaderMinLevel <= 60 &&
        config.leaderScaleLevel >= 1 && config.leaderScaleLevel <= 60;
}

bool Gate(ActivityServices const& services, ActivityKindId kind, bool gm)
{
    if (gm)
        return true;
    uint32 const required = kind == ActivityKindId::CapitalRaid ? 20 : 10;
    return kind != ActivityKindId::LeaderEncounter && services.config.leaderSpikeVerified &&
        services.leaderPrerequisitesMet && services.measuredLevel60Floor >= std::max(required, Minimum(services.config, kind));
}

LeaderScaleParameters Parameters(ActivityConfig const& config)
{
    return {uint8(config.leaderScaleLevel), config.leaderHealth, config.leaderGuardHealth,
            config.leaderDamagePercent, config.leaderGuardDamagePercent, config.leaderArmorPercent};
}

bool Isolated(ActivityServices const& services, ActivityPoint const& point)
{
    float const radius = std::max(ISOLATION_RADIUS, float(services.config.leaderGuardYards));
    for (Player* player : services.realPlayers)
        if (player && player->IsInWorld() && !player->IsGameMaster() && player->GetMapId() == point.mapId &&
            ((point.zoneId && player->GetZoneId() == point.zoneId) ||
             point.SqDist2d(player->GetPositionX(), player->GetPositionY()) <= radius * radius))
            return false;
    return true;
}

bool SafeMember(Player* bot, uint8 team, uint8 minLevel, Creature* target)
{
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    // Capital combat can set an ordinary PvP flag. The explicit PvE ruleset remains mandatory;
    // player/FFA/WarMode combat and every other unsafe state are still excluded.
    bool const safe = bot && bot->IsInWorld() && bot->GetMap() && !bot->GetMap()->Instanceable() &&
        bot->IsAlive() && !bot->GetVehicle() && !bot->IsInFlight() && !bot->duel &&
        !bot->IsBeingTeleported() && bot->HasAura(ActivityThreshold::SPELL_PVE) && !bot->IsFFAPvP() &&
        !bot->HasAura(ActivityThreshold::SPELL_HIGH_RISK) && !bot->HasAura(ActivityThreshold::SPELL_MERCENARY) &&
        !bot->HasAura(ActivityThreshold::SPELL_WAR_MODE) && !bot->InBattleground() && !bot->InArena();
    return ai && safe && bot->GetTeamId() == team &&
        bot->GetLevel() >= minLevel && bot->GetLevel() <= 60 && !bot->InBattlegroundQueue() &&
        !LocalLevelScaling::ScalingChoiceEnabled(bot) && (!target ||
        (bot->GetMap() == target->GetMap() && bot->InSamePhase(target) && bot->IsValidAttackTarget(target)));
}

bool Target(ActivityServices& services, ActivityCampaignTarget const& target, Creature*& leader,
            std::vector<Creature*>& guards, ActivityPoint& point, std::string& why, bool initial)
{
    leader = ActivityGuards::FindSpawn(target.spawnId);
    guards.clear();
    if (!leader || !leader->IsAlive() || !leader->IsInWorld() || !leader->GetMap() ||
        leader->GetMap()->Instanceable() || leader->GetSpawnId() != target.spawnId || leader->IsSummon() ||
        leader->GetCharmerOrOwnerGUID() || !std::isfinite(leader->GetPositionX()) ||
        !std::isfinite(leader->GetPositionY()) || !std::isfinite(leader->GetPositionZ()) ||
        (initial && (leader->IsInCombat() || leader->IsInEvadeMode() || !leader->IsFullHealth())))
    { why = "leader must be an alive loaded, healthy, unowned static overworld spawn"; return false; }
    point.mapId = leader->GetMapId();
    point.x = leader->GetPositionX();
    point.y = leader->GetPositionY();
    point.z = leader->GetPositionZ();
    point.zoneId = leader->GetZoneId();
    point.set = true;
    if (!Isolated(services, point) || ActivityGuards::QuestTargetNeeded(leader->GetEntry(), services.realPlayers, nullptr) ||
        ActivityGuards::QuestGiverHeld(leader->GetEntry(), services.realPlayers))
    { why = "an ordinary player or a held quest prevents the isolated leader test"; return false; }
    if (target.guardSpawns.size() > MAX_GUARDS)
    { why = "too many explicit guard spawns"; return false; }
    std::set<uint32> seen{target.spawnId};
    for (uint32 spawn : target.guardSpawns)
    {
        Creature* guard = ActivityGuards::FindSpawn(spawn);
        float const radius = float(services.config.leaderGuardYards);
        if (!spawn || !seen.insert(spawn).second || !guard || !guard->IsInWorld() || !guard->IsAlive() ||
            guard->GetMap() != leader->GetMap() || !guard->InSamePhase(leader) || guard->GetSpawnId() != spawn ||
            guard->IsSummon() || guard->GetCharmerOrOwnerGUID() || guard->GetExactDist2d(leader) > radius ||
            (initial && (guard->IsInCombat() || !guard->IsFullHealth())) ||
            ActivityGuards::QuestTargetNeeded(guard->GetEntry(), services.realPlayers, nullptr) ||
            ActivityGuards::QuestGiverHeld(guard->GetEntry(), services.realPlayers))
        { why = "every explicitly configured guard must be alive, loaded, nearby, compatible and quest-safe"; return false; }
        guards.push_back(guard);
    }
    return true;
}

bool SameTarget(ActivityCampaignTarget const& a, ActivityCampaignTarget const& b)
{
    return a.name == b.name && a.team == b.team && a.spawnId == b.spawnId && a.guardSpawns == b.guardSpawns;
}

bool SameParameters(LeaderScaleParameters const& a, LeaderScaleParameters const& b)
{
    return a.level == b.level && a.health == b.health && a.guardHealth == b.guardHealth &&
        a.damagePercent == b.damagePercent && a.guardDamagePercent == b.guardDamagePercent &&
        a.armorPercent == b.armorPercent;
}

void StopBots(Activity& activity)
{
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot)
            if (Player* bot = ActivityWorld::FindMember(member))
            {
                bot->AttackStop();
                bot->InterruptNonMeleeSpells(false);
            }
}

std::string WithdrawalReason(LeaderEncounterState const& state)
{
    return "; initiating outcome " + std::string(ActivityOutcomeName(state.withdrawOutcome)) + ": " + state.withdrawSummary;
}

void Finish(ActivityServices& services, Activity& activity, ActivityOutcome outcome, std::string why)
{
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    StopBots(activity);
    LeaderEncounterState* const state = StateOf(activity);
    if (state && state->leased && activity.phase != ActivityPhase::Resolving)
    {
        float const distance = LeaderScaler::BeginWithdrawal(activity.id);
        if (std::isfinite(distance) && distance > 0)
            state->withdrawal = ActivityGuards::GroundToward(state->spawn, state->rally.x, state->rally.y,
                                                            distance + 2 * RALLY_RADIUS);
        if (std::isfinite(distance) && distance > 0 && state->withdrawal.set &&
            state->withdrawal.mapId == state->spawn.mapId && std::isfinite(state->withdrawal.x) &&
            std::isfinite(state->withdrawal.y) && std::isfinite(state->withdrawal.z) &&
            state->spawn.SqDist2d(state->withdrawal.x, state->withdrawal.y) >
                (distance + RALLY_RADIUS) * (distance + RALLY_RADIUS))
        {
            state->withdrawOutcome = outcome;
            state->withdrawSummary = std::move(why);
            state->withdrawUntilMs = services.nowMs + 60000;
            activity.hardDeadlineMs = state->withdrawUntilMs + 1000;
            activity.SetCenter(state->withdrawal, RALLY_RADIUS);
            activity.SetTravel(ActivityTravel::None);
            activity.SetFlags(ACTIVITY_VIEW_RETREAT);
            activity.SetPhase(ActivityPhase::Resolving, services.nowMs);
            return;
        }
        outcome = ActivityOutcome::Guarded;
        why += "; no safe bounded withdrawal ground; immediate rollback cannot prevent later native area auras";
    }
    activity.Finish(outcome, std::move(why));
}

bool GroupReady(Activity& activity)
{
    Group* const group = activity.groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
    if (!group || group->GetLeaderGUID() != activity.leader)
        return false;
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot && !group->IsMember(member.guid))
            return false;
    return true;
}
}  // namespace

bool LeaderEncounterKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                               std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    ActivityConfig const& config = services.config;
    if (!Enabled(config, Id()) || !Gate(services, Id(), request.gm) ||
        !LeaderScalerRules::Valid(Parameters(config)) || LeaderScaler::HasLeases())
    { why = "leader campaign requires enabled config, measured spike/census gates, side-effect acknowledgement and a free lease"; return false; }
    uint32 const required = request.gm ? Minimum(config, Id()) :
        std::max(Minimum(config, Id()), Id() == ActivityKindId::CapitalRaid ? 20u : 10u);
    if (Maximum(config, Id()) < required)
    { why = "configured campaign maximum is below its gated minimum raid size"; return false; }
    uint32 const wanted = std::min(request.capacity, request.size ? std::clamp(request.size, required, Maximum(config, Id()))
                                                              : Maximum(config, Id()));
    if (wanted < required)
    { why = "insufficient participant capacity for the configured leader test"; return false; }
    for (ActivityCampaignTarget const& target : ConfiguredTargets(config, Id()))
    {
        if ((!request.target.empty() && target.name != request.target) ||
            (request.team && *request.team != target.team) || target.team > TEAM_HORDE)
            continue;
        Creature* npc = nullptr;
        std::vector<Creature*> guards;
        ActivityPoint point;
        if (!Target(services, target, npc, guards, point, why, true))
            continue;
        ActivityPoint rally;
        if (target.rally.set)
        {
            if (target.rally.mapId != point.mapId || !std::isfinite(target.rally.x) || !std::isfinite(target.rally.y) ||
                !std::isfinite(target.rally.z))
            { why = "invalid configured leader rally"; continue; }
            rally = ActivityWorld::GroundNear(target.rally, 0);
        }
        else
            rally = ActivityGuards::GroundToward(point, point.x + 1.0f, point.y, RALLY_DISTANCE);
        if (!rally.set || rally.mapId != point.mapId || point.SqDist2d(rally.x, rally.y) < 80.0f * 80.0f)
        { why = "no safe ground rally outside the leader's immediate aggro range"; continue; }
        ActivityCandidateQuery query;
        query.team = target.team;
        query.minLevel = request.gm ? uint8(config.leaderMinLevel) : 60;
        query.maxLevel = 60;
        query.center = rally;
        query.sameMapOnly = true;
        query.maxDistance = float(config.leaderNearbyYards);
        query.limit = wanted * 2;
        query.rulesetSafe = true;
        auto found = services.candidates.Find(query);
        found.erase(std::remove_if(found.begin(), found.end(), [&](ActivityCandidate const& candidate)
        {
            ActivityMember member;
            member.guid = candidate.guid;
            member.epoch = candidate.epoch;
            return !SafeMember(ActivityWorld::FindMember(member), target.team, query.minLevel, npc);
        }), found.end());
        if (found.size() < required)
        { why = "too few nearby compatible bots with native attack permission and no per-viewer scaling"; continue; }
        auto state = std::make_unique<LeaderEncounterState>();
        state->target = target;
        state->spawn = point;
        state->npc = npc->GetGUID();
        state->npcEntry = npc->GetEntry();
        state->required = required;
        state->parameters = Parameters(config);
        plan.kind = Id();
        plan.gm = request.gm;
        plan.team = target.team;
        plan.minLevel = query.minLevel;
        plan.maxLevel = 60;
        plan.target = target.name;
        plan.name = ActivityGuards::CreatureName(npc->GetEntry());
        plan.rally = rally;
        plan.wanted = wanted;
        plan.needed = required;
        plan.leaderFixed = true;
        plan.candidates = std::move(found);
        plan.decisions.clear();
        plan.state = std::move(state);
        return true;
    }
    if (why.empty())
        why = "no explicit compatible leader target";
    return false;
}

std::optional<std::vector<std::string>> LeaderEncounterKind::Targets(ActivityServices& services,
                                                                   ActivityStartRequest const& request)
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<std::string> result;
    if (!Enabled(services.config, Id()) || !Gate(services, Id(), request.gm) || LeaderScaler::HasLeases())
        return result;
    for (ActivityCampaignTarget const& target : ConfiguredTargets(services.config, Id()))
    {
        if (request.team && *request.team != target.team)
            continue;
        Creature* npc = nullptr;
        std::vector<Creature*> guards;
        ActivityPoint point;
        std::string why;
        if (target.team <= TEAM_HORDE && Target(services, target, npc, guards, point, why, true))
            result.push_back(target.name);
        if (result.size() == 4)
            break;
    }
    return result;
}

bool LeaderEncounterKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    LeaderEncounterState* state = dynamic_cast<LeaderEncounterState*>(plan.state.get());
    ActivityCampaignTarget const* configured = Configured(services.config, Id(), plan.target);
    if (!Enabled(services.config, Id()) || !Gate(services, Id(), plan.gm) || !state || !configured ||
        !SameTarget(*configured, state->target) ||
        !SameParameters(Parameters(services.config), state->parameters) ||
        LeaderScaler::HasLeases() || plan.candidates.empty() || plan.decisions.empty() || plan.decisions[0] != 1)
        return false;
    Creature* npc = nullptr;
    std::vector<Creature*> guards;
    ActivityPoint point;
    std::string why;
    if (!Target(services, *configured, npc, guards, point, why, true) || npc->GetGUID() != state->npc ||
        npc->GetEntry() != state->npcEntry)
        return false;
    std::vector<ActivityCandidate const*> accepted;
    for (std::size_t i = 0; i < plan.candidates.size() && accepted.size() < plan.wanted; ++i)
    {
        if (i >= plan.decisions.size() || plan.decisions[i] != 1)
            continue;
        ActivityCandidate const& candidate = plan.candidates[i];
        ActivityMember member;
        member.guid = candidate.guid;
        member.epoch = candidate.epoch;
        Player* bot = ActivityWorld::FindMember(member);
        // The director already rechecked identity and membership; its held set still contains this plan.
        if (!ActivityCandidates::IsFree(bot) || bot->IsInCombat() ||
            !SafeMember(bot, plan.team, plan.minLevel, npc))
            continue;
        if (i == 0 || !accepted.empty())
            accepted.push_back(&candidate);
    }
    if (accepted.size() < state->required || accepted.front()->guid != plan.candidates.front().guid)
        return false;
    activity.leader = accepted.front()->guid;
    activity.name = plan.name;
    activity.target = plan.target;
    activity.minLevel = plan.minLevel;
    activity.maxLevel = plan.maxLevel;
    for (ActivityCandidate const* candidate : accepted)
    {
        activity.Enroll(candidate->guid, candidate->epoch, true, candidate->level, services.nowUnix);
        if (candidate->guid != activity.leader)
            state->toAdd.push_back(candidate->guid);
    }
    state->formUntilMs = services.nowMs + ActivityThreshold::FORM_TIMEOUT_MS;
    state->gatherUntilMs = state->formUntilMs + uint64(services.config.leaderGatherSeconds) * 1000;
    state->fightUntilMs = state->gatherUntilMs + uint64(services.config.leaderMinutes) * 60000;
    state->rally = plan.rally;
    activity.hardDeadlineMs = state->fightUntilMs;
    activity.SetCenter(plan.rally, RALLY_RADIUS);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    activity.SetPhase(ActivityPhase::Forming, services.nowMs);
    activity.state = std::move(plan.state);
    return true;
}

void LeaderEncounterKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;
    LeaderEncounterState* const state = StateOf(activity);
    if (state && activity.phase == ActivityPhase::Resolving)
    {
        bool arrived = true;
        for (ActivityMember const& member : activity.members)
            if (member.In() && member.isBot)
            {
                Player* const bot = ActivityWorld::FindMember(member);
                if (bot && !bot->IsAlive())
                    continue;
                if (!bot || !SafeMember(bot, activity.team, activity.minLevel, nullptr) ||
                    bot->GetMapId() != state->withdrawal.mapId)
                {
                    activity.Finish(ActivityOutcome::Guarded, "leader withdrawal member became unavailable or unsafe; immediate rollback" +
                                    WithdrawalReason(*state));
                    return;
                }
                arrived = arrived && !bot->IsInCombat() &&
                    state->withdrawal.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) <= RALLY_RADIUS * RALLY_RADIUS &&
                    std::fabs(bot->GetPositionZ() - state->withdrawal.z) <= RALLY_RADIUS;
            }
        if (services.nowMs >= state->withdrawUntilMs)
            activity.Finish(ActivityOutcome::Timeout, "leader withdrawal exceeded 60 seconds; immediate rollback cannot prevent later native area auras" +
                            WithdrawalReason(*state));
        else if (arrived && LeaderScaler::WithdrawalClear(activity.id))
            activity.Finish(state->withdrawOutcome, state->withdrawSummary + "; raid withdrew beyond leased spawns and area-aura range");
        return;
    }
    ActivityCampaignTarget const* const configured = Configured(services.config, Id(), activity.target);
    if (!state || !Enabled(services.config, Id()) || !Gate(services, Id(), activity.gm) ||
        !configured || !SameTarget(*configured, state->target) ||
        !SameParameters(Parameters(services.config), state->parameters))
    { Finish(services, activity, ActivityOutcome::Guarded, "leader encounter configuration changed"); return; }
    if (state->leased)
    {
        std::string why;
        LeaderScaleResult const result = LeaderScaler::Poll(activity.id, why);
        if (result != LeaderScaleResult::Active)
        {
            bool const success = result == LeaderScaleResult::Killed &&
                (Id() != ActivityKindId::LeaderEncounter || state->acceptanceQualified);
            Finish(services, activity, success ? ActivityOutcome::Success : ActivityOutcome::Guarded,
                   why + (result == LeaderScaleResult::Killed && !success ?
                       "; diagnostic kill does not qualify as a 20+ exact-level-60 bot spike" : ""));
            return;
        }
    }
    Creature* npc = ActivityGuards::FindSpawn(state->target.spawnId);
    if (!npc || !npc->IsAlive() || npc->GetGUID() != state->npc || npc->GetEntry() != state->npcEntry ||
        !Isolated(services, state->spawn))
    { Finish(services, activity, ActivityOutcome::Guarded, "leader instance disappeared or an ordinary player entered the test area"); return; }
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot)
            if (Player* const bot = ActivityWorld::FindMember(member); bot && !bot->IsAlive())
            {
                activity.MarkLeft(member.guid, ActivityLeaveReason::Dropped, services.nowUnix);
                if (member.guid == activity.leader)
                { Finish(services, activity, ActivityOutcome::LeaderGone, "leader raid lost its bot leader"); return; }
            }
    if (activity.BotsIn() < state->required)
    { Finish(services, activity, ActivityOutcome::TooFew, "leader test dropped below its required raid size"); return; }
    bool ready = true;
    std::vector<LeaderScaleParticipant> participants;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || !member.isBot)
            continue;
        Player* bot = ActivityWorld::FindMember(member);
        if (!SafeMember(bot, activity.team, activity.minLevel, npc))
        { Finish(services, activity, ActivityOutcome::Guarded, "leader test member became unsafe or changed lifetime/map"); return; }
        ready = ready && bot->GetMapId() == activity.center.mapId && !bot->IsInCombat() &&
            activity.center.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) <= RALLY_RADIUS * RALLY_RADIUS &&
            std::fabs(bot->GetPositionZ() - activity.center.z) <= RALLY_RADIUS;
        participants.push_back({member.guid, member.epoch});
    }
    if (activity.phase == ActivityPhase::Forming)
    {
        if (services.nowMs >= state->formUntilMs)
        { Finish(services, activity, ActivityOutcome::Timeout, "leader raid formation timed out"); return; }
        if (activity.groupGuid.IsEmpty())
        {
            std::vector<ObjectGuid> added, skipped;
            ActivityOpResult const result = services.ops.FormGroup(activity.leader, state->toAdd, GROUP_LOOT,
                                                                   ITEM_QUALITY_UNCOMMON, activity.groupGuid, added, skipped);
            if (result == ActivityOpResult::Deferred)
                return;
            if (result != ActivityOpResult::Done)
            { Finish(services, activity, ActivityOutcome::Guarded, "leader raid group creation failed"); return; }
            state->toAdd = std::move(skipped);
            activity.membershipDirty = true;
        }
        for (auto it = state->toAdd.begin(); it != state->toAdd.end();)
        {
            ActivityOpResult const result = services.ops.AddMember(activity.groupGuid, activity.leader, *it);
            if (result == ActivityOpResult::Deferred)
                return;
            if (result != ActivityOpResult::Done)
            { Finish(services, activity, ActivityOutcome::Guarded, "leader raid member could not be added"); return; }
            it = state->toAdd.erase(it);
        }
        if (!GroupReady(activity))
            return;
        activity.SetFlags(0);  // map-thread rally travel is allowed only after group formation completes
        activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
        return;
    }
    if (activity.phase == ActivityPhase::Gathering)
    {
        if (services.nowMs >= state->gatherUntilMs)
        { Finish(services, activity, ActivityOutcome::Timeout, "leader raid did not reach its rally"); return; }
        if (!ready || !GroupReady(activity))
            return;
        state->acceptanceQualified = participants.size() >= 20 &&
            std::all_of(participants.begin(), participants.end(), [](LeaderScaleParticipant const& member)
            {
                Player* const bot = ObjectAccessor::FindConnectedPlayer(member.guid);
                return bot && bot->GetLevel() == 60;
            });
        Creature* checked = nullptr;
        std::vector<Creature*> guards;
        ActivityPoint point;
        std::string why;
        if (!Target(services, state->target, checked, guards, point, why, true) || checked != npc ||
            !LeaderScaler::Begin(activity.id, npc, guards, participants, state->parameters, why, Id()))
        { Finish(services, activity, ActivityOutcome::Guarded, "leader scale refused: " + why); return; }
        state->leased = true;
        state->fightUntilMs = std::min(activity.hardDeadlineMs, services.nowMs + uint64(services.config.leaderMinutes) * 60000);
        activity.SetCenter(point, 40.0f);  // everybody walks inside the map-thread objective's 60-yard reach
        activity.SetObjective(npc->GetGUID());
        activity.SetFlags(0);
        activity.SetPhase(ActivityPhase::Active, services.nowMs);
        services.ops.PostToChannel(activity.leader, ActivityChatTarget::Party,
            ActivityWorld::Line("coa_act_raid_charge", "Charge! Take down %target!", {{"%target", activity.name}}));
        return;
    }
    if (activity.phase == ActivityPhase::Active && services.nowMs >= state->fightUntilMs)
        Finish(services, activity, ActivityOutcome::Timeout, "leader combat deadline expired");
}

void LeaderEncounterKind::OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member)
{
    COA_ASSERT_WORLD_THREAD();
    LeaderEncounterState const* const state = StateOf(activity);
    if (activity.phase == ActivityPhase::Resolving)
        return;
    if (member.guid == activity.leader || !state || activity.BotsIn() < state->required)
        Finish(services, activity, ActivityOutcome::TooFew, "leader raid lost its leader or required raid size");
}

std::string LeaderEncounterKind::Join(ActivityServices&, Activity&, Player*)
{
    COA_ASSERT_WORLD_THREAD();
    return "This is an isolated GM bot encounter; ordinary player enrollment is unavailable.";
}

void LeaderEncounterKind::OnEnd(ActivityServices&, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    StopBots(activity);
    LeaderScaler::Restore(activity.id);
}

std::string LeaderEncounterKind::Describe(Activity const& activity, uint64 nowMs) const
{
    COA_ASSERT_WORLD_THREAD();
    LeaderEncounterState const* const state = dynamic_cast<LeaderEncounterState const*>(activity.state.get());
    if (!state)
        return "leader encounter has no originating state";
    uint64 const deadline = activity.phase == ActivityPhase::Resolving ? state->withdrawUntilMs : state->fightUntilMs;
    uint64 const left = deadline > nowMs ? (deadline - nowMs) / 1000 : 0;
    auto const lease = LeaderScaler::Describe(activity.id);
    return std::string(ActivityKindName(Id())) + " " + state->target.name + " spawn " + std::to_string(state->target.spawnId) + ", " +
        std::to_string(state->target.guardSpawns.size()) + " explicit guards, " + std::to_string(state->required) +
        " required bots, " + std::to_string(left) + " s left; " + lease.front() +
        (activity.phase == ActivityPhase::Resolving ? "; resolving withdrawal" + WithdrawalReason(*state) : "");
}

std::unique_ptr<ActivityKind> CreateLeaderEncounterKind() { return std::make_unique<LeaderEncounterKind>(); }
std::unique_ptr<ActivityKind> CreateCapitalRaidKind()
{
    return std::make_unique<LeaderEncounterKind>(ActivityKindId::CapitalRaid);
}
std::unique_ptr<ActivityKind> CreateLieutenantRaidKind()
{
    return std::make_unique<LeaderEncounterKind>(ActivityKindId::LieutenantRaid);
}
