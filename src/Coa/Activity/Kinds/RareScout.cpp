/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RareScout.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "ActivityPvePolicy.h"
#include "CoaPlayerPrefs.h"
#include "CoaThread.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
constexpr char const* LOG = "playerbots.activity";
constexpr float CAMP_OFFSET_YARDS = 80.0f;
constexpr float CAMP_RADIUS = 25.0f;
constexpr float CAMP_LEASH = 40.0f;
constexpr float SAFE_RARE_YARDS = 45.0f;
constexpr float SIGHT_YARDS = 120.0f;
constexpr uint64 POST_RETRY_MS = 10000;
constexpr std::size_t MAX_TARGETS = 4;

RareScoutState* StateOf(Activity& activity) { return dynamic_cast<RareScoutState*>(activity.state.get()); }

bool CheckSpawn(ActivityHuntTarget const& target, ActivitySpawnInfo& spawn)
{
    CreatureData const* data = sObjectMgr->GetCreatureData(target.spawnId);
    CreatureTemplate const* info = data ? sObjectMgr->GetCreatureTemplate(data->id) : nullptr;
    MapEntry const* map = data ? sMapStore.LookupEntry(data->mapid) : nullptr;
    if (!info || !data->phaseMask || !std::isfinite(data->posX) || !std::isfinite(data->posY) ||
        !std::isfinite(data->posZ) || !RareScoutRules::AcceptsTemplate(info->rank, info->minlevel, info->maxlevel,
                                                ((info->npcflag | data->npcflag) & UNIT_NPC_FLAG_QUESTGIVER) != 0,
                                                map && !map->Instanceable()))
        return false;
    return ActivityGuards::DescribeSpawn(target.spawnId, spawn);
}

std::pair<uint8, uint8> LevelBand(ActivitySpawnInfo const& spawn, ActivityConfig const& config,
                                 ActivityStartRequest const& request)
{
    uint8 const low = uint8(std::max<int32>(1, int32(spawn.level) - int32(config.rareScoutLevelSpread)));
    uint8 const high = uint8(std::min<uint32>(60, uint32(spawn.level) + config.rareScoutLevelSpread));
    if (request.level)
        return request.level >= low && request.level <= high ? std::pair{request.level, request.level}
                                                             : std::pair<uint8, uint8>{0, 0};
    return {low, high};
}

ObjectGuid Audience(ActivityServices const& services, uint8 team, ActivitySpawnInfo const& spawn)
{
    CoaPlayerPrefs const& prefs = CoaPlayerPrefs::Instance();
    CreatureData const* data = sObjectMgr->GetCreatureData(spawn.spawnId);
    if (!prefs.IsLoaded() || !data)
        return ObjectGuid::Empty;
    ActivityPoint const& point = spawn.point;
    float bestSq = float(services.config.rareScoutNearbyYards) * services.config.rareScoutNearbyYards;
    ObjectGuid found;
    for (Player* player : services.realPlayers)
    {
        if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || player->IsGameMaster() ||
            player->isAFK() || player->GetTeamId() != team || player->GetMapId() != point.mapId ||
            !player->InSamePhase(data->phaseMask) || !player->GetSession() || player->GetSession()->IsBot() ||
            player->GetSession()->isLogingOut() ||
            prefs.VoiceOptedOut(player->GetSession()->GetAccountId()))
            continue;
        float const sq = point.SqDist2d(player->GetPositionX(), player->GetPositionY());
        if (sq <= bestSq)
        {
            bestSq = sq;
            found = player->GetGUID();
        }
    }
    return found;
}

bool EligibleBot(Player* bot)
{
    return ActivityCandidates::IsFree(bot) && ActivityPvePolicy::RareSafe(bot);
}

bool SafeBot(Player* bot) { return EligibleBot(bot) && !bot->IsInCombat(); }

bool InSpawnPhase(Player* bot, uint32 spawnId)
{
    CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
    return bot && data && bot->InSamePhase(data->phaseMask);
}

std::vector<ActivityCandidate> Candidates(ActivityServices& services, ActivityStartRequest const& request,
                                          ActivityHuntTarget const& target, ActivitySpawnInfo const& spawn)
{
    auto const [low, high] = LevelBand(spawn, services.config, request);
    if (!low || (!request.gm && Audience(services, target.team, spawn).IsEmpty()))
        return {};
    ActivityCandidateQuery query;
    query.team = target.team;
    query.minLevel = low;
    query.maxLevel = high;
    query.center = spawn.point;
    query.sameMapOnly = true;
    query.maxDistance = float(services.config.rareScoutNearbyYards);
    query.rulesetSafe = true;
    query.limit = 8;
    auto found = services.candidates.Find(query);
    found.erase(std::remove_if(found.begin(), found.end(), [&](ActivityCandidate const& candidate)
    {
        ActivityMember member;
        member.guid = candidate.guid;
        member.epoch = candidate.epoch;
        Player* bot = ActivityWorld::FindMember(member);
        return !SafeBot(bot) || !InSpawnPhase(bot, spawn.spawnId);
    }), found.end());
    if (found.size() > 2)
        found.resize(2);
    return found;
}

ActivityPoint CampOf(ActivitySpawnInfo const& spawn, ActivityCandidate const& candidate)
{
    float dx = candidate.x - spawn.point.x;
    float dy = candidate.y - spawn.point.y;
    if (dx * dx + dy * dy < 1.0f)
    {
        dx = 1.0f;
        dy = 0.0f;
    }
    return ActivityGuards::GroundToward(spawn.point, spawn.point.x + dx, spawn.point.y + dy, CAMP_OFFSET_YARDS);
}

bool InCamp(Player* bot, RareScoutState const& state, float radius)
{
    return bot && bot->GetMapId() == state.camp.mapId &&
           state.camp.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) <= radius * radius &&
           std::fabs(bot->GetPositionZ() - state.camp.z) <= CAMP_RADIUS;
}

bool LiveRare(Creature* creature, RareScoutState const& state)
{
    CreatureTemplate const* info = creature ? creature->GetCreatureTemplate() : nullptr;
    return creature && creature->IsInWorld() && creature->GetMapId() == state.spawn.point.mapId &&
           creature->GetEntry() == state.spawn.entry && info &&
           RareScoutRules::AcceptsTemplate(info->rank, creature->GetLevel(), creature->GetLevel(),
                                           (creature->GetNpcFlags() & UNIT_NPC_FLAG_QUESTGIVER) != 0, true);
}

bool Escaped(Player* bot, RareScoutState const& state)
{
    float const distanceSq = state.escapeMinDistance * state.escapeMinDistance;
    if (!state.escape.set || bot->GetMapId() != state.escape.mapId ||
        state.escape.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) >
            ActivityThreshold::RALLY_ARRIVE_RADIUS * ActivityThreshold::RALLY_ARRIVE_RADIUS ||
        std::fabs(bot->GetPositionZ() - state.escape.z) > CAMP_RADIUS ||
        state.spawn.point.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) <= distanceSq)
        return false;
    Creature* const rare = ActivityGuards::FindSpawn(state.spawn.spawnId);
    return !rare || !rare->IsAlive() || (LiveRare(rare, state) && bot->GetExactDist2d(rare) > state.escapeMinDistance);
}

void Retire(ActivityServices& services, Activity& activity, RareScoutState& state, Player* bot,
            ActivityOutcome outcome, std::string summary)
{
    float const sight = sPlayerbotAIConfig.sightDistance;
    if (!std::isfinite(sight))
    {
        activity.Finish(ActivityOutcome::Guarded, "rare scout has no finite sight distance for withdrawal");
        return;
    }
    state.escapeMinDistance = std::max(200.0f, sight + 50.0f);
    Creature* const rare = ActivityGuards::FindSpawn(state.spawn.spawnId);
    bool const live = rare && rare->IsAlive() && LiveRare(rare, state);
    float const originX = live ? rare->GetPositionX() : state.spawn.point.x;
    float const originY = live ? rare->GetPositionY() : state.spawn.point.y;
    float dx = bot->GetPositionX() - originX;
    float dy = bot->GetPositionY() - originY;
    if (dx * dx + dy * dy < 1.0f)
    {
        dx = state.camp.x - state.spawn.point.x;
        dy = state.camp.y - state.spawn.point.y;
    }
    float const displacement = std::sqrt(state.spawn.point.SqDist2d(originX, originY));
    float const offset = state.escapeMinDistance + displacement + ActivityThreshold::RALLY_ARRIVE_RADIUS + 30.0f;
    state.escape = ActivityGuards::GroundToward(state.spawn.point, state.spawn.point.x + dx,
                                               state.spawn.point.y + dy, offset);
    float const minCenter = state.escapeMinDistance + ActivityThreshold::RALLY_ARRIVE_RADIUS;
    if (!state.escape.set || state.escape.mapId != state.spawn.point.mapId || !std::isfinite(state.escape.x) ||
        !std::isfinite(state.escape.y) || !std::isfinite(state.escape.z) ||
        state.spawn.point.SqDist2d(state.escape.x, state.escape.y) <= minCenter * minCenter ||
        (live && (state.escape.x - originX) * (state.escape.x - originX) +
                     (state.escape.y - originY) * (state.escape.y - originY) <= minCenter * minCenter))
    {
        activity.Finish(ActivityOutcome::Guarded, "rare scout has no safe ground point for withdrawal");
        return;
    }
    state.retireOutcome = outcome;
    state.retireSummary = std::move(summary);
    state.retireUntilMs = std::min(services.nowMs + RareScoutRules::RETIRE_MS, activity.hardDeadlineMs);
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetCenter(state.escape, ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_RETREAT);
    activity.SetPhase(ActivityPhase::Resolving, services.nowMs);
}

void Retiring(ActivityServices& services, Activity& activity, RareScoutState& state, Player* bot)
{
    RareScoutProgress const progress = RareScoutRules::RetirementAt(services.nowMs, state.retireUntilMs,
                                                                   Escaped(bot, state), bot->IsInCombat());
    if (progress == RareScoutProgress::Success)
        activity.Finish(state.retireOutcome, state.retireSummary + "; withdrew beyond the rare's sight range");
    else if (progress == RareScoutProgress::Timeout)
        activity.Finish(ActivityOutcome::Timeout, "rare scout withdrawal expired before a safe non-combat arrival");
}

void Announce(ActivityServices& services, Activity& activity, RareScoutState& state, Player* bot)
{
    if (state.announced || services.nowMs < state.nextPostMs ||
        services.config.chatChannel != ActivityChatChannel::Zone || !activity.CanPost(services.config, services.nowMs))
        return;
    ObjectGuid const audience = Audience(services, activity.team, state.spawn);
    if (!activity.gm && audience.IsEmpty())
        return;
    std::string area = ActivityWorld::AreaName(state.spawn.point.zoneId);
    if (area.empty())
        area = PlayerbotTextMgr::instance().GetLocalePriority() == LOCALE_frFR ? "ici" : "here";
    char const* fallback = PlayerbotTextMgr::instance().GetLocalePriority() == LOCALE_frFR
        ? "J'ai aperçu %target vers %area." : "I spotted %target near %area.";
    std::string const line = ActivityWorld::Line("coa_act_rare_seen", fallback,
        {{"%target", ActivityGuards::CreatureName(state.spawn.entry)}, {"%area", area}});
    ActivityOpResult const result = services.ops.PostToChannels(bot->GetGUID(), services.config.chatChannels,
                                                               line, state.spawn.point.zoneId, audience);
    if (result == ActivityOpResult::Done)
    {
        state.announced = true;
        activity.NotePost(services.nowMs);
    }
    else
        state.nextPostMs = services.nowMs + POST_RETRY_MS;
}
}  // namespace

std::optional<std::vector<std::string>> RareScoutKind::Targets(ActivityServices& services,
                                                             ActivityStartRequest const& request)
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<std::string> names;
    if (!services.config.rareScoutMax || spawnInUse.size() >= services.config.rareScoutMax)
        return names;
    for (ActivityHuntTarget const& target : services.config.rareScoutTargets)
    {
        if ((request.team && *request.team != target.team) || spawnInUse.count(target.spawnId) ||
            (!request.target.empty() && request.target != target.name))
            continue;
        ActivitySpawnInfo spawn;
        if (CheckSpawn(target, spawn) && !Candidates(services, request, target, spawn).empty())
            names.push_back(target.name);
        if (names.size() >= MAX_TARGETS)
            break;
    }
    return names;
}

bool RareScoutKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                         std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    if (!request.capacity || !services.config.rareScoutMax || spawnInUse.size() >= services.config.rareScoutMax)
    {
        why = "no rare scout slot or participant place available";
        return false;
    }
    for (ActivityHuntTarget const& target : services.config.rareScoutTargets)
    {
        if ((request.team && *request.team != target.team) || spawnInUse.count(target.spawnId) ||
            (!request.target.empty() && request.target != target.name))
            continue;
        ActivitySpawnInfo spawn;
        if (!CheckSpawn(target, spawn))
            continue;
        auto found = Candidates(services, request, target, spawn);
        if (found.empty())
            continue;
        ActivityPoint const camp = CampOf(spawn, found.front());
        if (!camp.set || camp.mapId != spawn.point.mapId || !std::isfinite(camp.x) || !std::isfinite(camp.y) ||
            !std::isfinite(camp.z) || spawn.point.SqDist2d(camp.x, camp.y) < SAFE_RARE_YARDS * SAFE_RARE_YARDS)
            continue;
        auto state = std::make_unique<RareScoutState>();
        state->spawn = spawn;
        state->camp = camp;
        auto const [low, high] = LevelBand(spawn, services.config, request);
        plan.kind = ActivityKindId::RareScout;
        plan.gm = request.gm;
        plan.team = target.team;
        plan.minLevel = low;
        plan.maxLevel = high;
        plan.rally = camp;
        plan.target = target.name;
        plan.name = ActivityGuards::CreatureName(spawn.entry);
        plan.wanted = 1;
        plan.needed = 1;
        plan.leaderFixed = false;
        plan.candidates = std::move(found);
        plan.decisions.clear();
        plan.state = std::move(state);
        return true;
    }
    why = "no free whitelisted open-world rare of level 1-60 with a nearby safe bot and audience";
    return false;
}

bool RareScoutKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    RareScoutState* state = dynamic_cast<RareScoutState*>(plan.state.get());
    ActivityHuntTarget const* target = services.config.FindRareScoutTarget(plan.target);
    ActivitySpawnInfo current;
    if (!state || !target || target->spawnId != state->spawn.spawnId || target->team != plan.team ||
        !services.config.rareScoutMax || spawnInUse.size() >= services.config.rareScoutMax ||
        spawnInUse.count(state->spawn.spawnId) || !CheckSpawn(*target, current) ||
        current.entry != state->spawn.entry || (!plan.gm && Audience(services, plan.team, current).IsEmpty()))
        return false;
    ActivityCandidate const* accepted = nullptr;
    for (std::size_t i = 0; i < plan.candidates.size(); ++i)
    {
        if (i >= plan.decisions.size() || plan.decisions[i] != 1)
            continue;
        ActivityCandidate const& candidate = plan.candidates[i];
        ActivityMember member;
        member.guid = candidate.guid;
        member.epoch = candidate.epoch;
        Player* bot = ActivityWorld::FindMember(member);
        float const range = float(services.config.rareScoutNearbyYards);
        if (SafeBot(bot) && InSpawnPhase(bot, current.spawnId) && bot->GetMapId() == current.point.mapId &&
            bot->GetLevel() >= plan.minLevel &&
            bot->GetLevel() <= plan.maxLevel && current.point.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) <=
                range * range)
        {
            accepted = &candidate;
            break;
        }
    }
    if (!accepted)
        return false;
    activity.name = plan.name;
    activity.target = plan.target;
    activity.minLevel = plan.minLevel;
    activity.maxLevel = plan.maxLevel;
    activity.Enroll(accepted->guid, accepted->epoch, true, accepted->level, services.nowUnix);
    state->scout = accepted->guid;
    state->scoutEpoch = accepted->epoch;
    state->waitUntilMs = services.nowMs + uint64(services.config.rareScoutMinutes) * 60000;
    activity.hardDeadlineMs = state->waitUntilMs + RareScoutRules::RETIRE_MS;
    activity.SetCenter(state->camp, CAMP_RADIUS);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
    spawnInUse[state->spawn.spawnId] = activity.id;
    activity.state = std::move(plan.state);
    return true;
}

void RareScoutKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;
    RareScoutState* state = StateOf(activity);
    ActivityMember const* member = state ? activity.FindMember(state->scout) : nullptr;
    Player* bot = member && member->In() ? ActivityWorld::FindMember(*member) : nullptr;
    if (!state || !member || !member->In() || member->epoch != state->scoutEpoch || !EligibleBot(bot) ||
        !InSpawnPhase(bot, state->spawn.spawnId) || bot->GetMapId() != state->spawn.point.mapId ||
        bot->GetLevel() < activity.minLevel ||
        bot->GetLevel() > activity.maxLevel)
    {
        activity.Finish(ActivityOutcome::Guarded, "rare scout left or became unsafe");
        return;
    }
    if (state->retireOutcome != ActivityOutcome::None)
    {
        Retiring(services, activity, *state, bot);
        return;
    }
    if (bot->IsInCombat())
    {
        Retire(services, activity, *state, bot, ActivityOutcome::Guarded, "rare scout met NPC combat pressure");
        return;
    }
    if (services.nowMs >= state->waitUntilMs)
    {
        Retire(services, activity, *state, bot, ActivityOutcome::Timeout, "rare scout reached its observation limit");
        return;
    }
    if (!activity.gm && Audience(services, activity.team, state->spawn).IsEmpty())
    {
        activity.Finish(ActivityOutcome::NoPlayers, "no nearby audience for the rare scout");
        return;
    }
    if (activity.phase == ActivityPhase::Gathering)
    {
        if (!InCamp(bot, *state, CAMP_RADIUS))
            return;
        activity.SetPhase(ActivityPhase::Active, services.nowMs);
        activity.SetFlags(ACTIVITY_VIEW_QUIET);
    }
    if (!InCamp(bot, *state, CAMP_LEASH))
    {
        activity.Finish(ActivityOutcome::Guarded, "rare scout left its observation camp");
        return;
    }
    Creature* creature = ActivityGuards::FindSpawn(state->spawn.spawnId);
    state->observation = !creature ? RareScoutObservation::Unloaded :
        (creature->IsAlive() ? RareScoutObservation::Alive : RareScoutObservation::Dead);
    if (creature && !LiveRare(creature, *state))
    {
        activity.Finish(ActivityOutcome::Guarded, "rare spawn changed to an unsafe creature");
        return;
    }
    if (creature && creature->IsAlive() && bot->GetExactDist2d(creature) < SAFE_RARE_YARDS)
    {
        Retire(services, activity, *state, bot, ActivityOutcome::Guarded, "rare moved too close to the camp");
        return;
    }
    bool const visible = creature && creature->IsAlive() && bot->GetExactDist(creature) <= SIGHT_YARDS &&
                         bot->CanSeeOrDetect(creature, false, true) && bot->IsWithinLOSInMap(creature);
    if (visible)
    {
        if (!state->sightedAtMs)
        {
            state->sightedAtMs = services.nowMs;
            state->seenCreature = creature->GetGUID();
            LOG_DEBUG(LOG, "activity #{} rare: {} alive at spawn {}", activity.id, activity.target,
                      state->spawn.spawnId);
        }
        Announce(services, activity, *state, bot);
    }
    RareScoutProgress const progress = RareScoutRules::ProgressAt(services.nowMs, state->waitUntilMs,
                                                                 state->sightedAtMs, state->announced);
    if (progress == RareScoutProgress::Success)
        Retire(services, activity, *state, bot, ActivityOutcome::Success,
               Acore::StringFormat("{} spotted alive; announcement sent", activity.target));
    else if (progress == RareScoutProgress::Timeout)
        Retire(services, activity, *state, bot, ActivityOutcome::Timeout, "rare scout reached its observation limit");
}

void RareScoutKind::OnMemberLeft(ActivityServices&, Activity& activity, ActivityMember const& member)
{
    COA_ASSERT_WORLD_THREAD();
    RareScoutState const* state = StateOf(activity);
    if (state && state->scout == member.guid)
        activity.Finish(ActivityOutcome::TooFew, "the rare scout left");
}

std::string RareScoutKind::Join(ActivityServices&, Activity&, Player*)
{
    COA_ASSERT_WORLD_THREAD();
    return "The rare scout only watches the spawn; no group is forming.";
}

void RareScoutKind::OnEnd(ActivityServices&, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    RareScoutState const* state = StateOf(activity);
    if (!state)
        return;
    auto const held = spawnInUse.find(state->spawn.spawnId);
    if (held != spawnInUse.end() && held->second == activity.id)
        spawnInUse.erase(held);
}

std::string RareScoutKind::Describe(Activity const& activity, uint64 nowMs) const
{
    RareScoutState const* state = dynamic_cast<RareScoutState const*>(activity.state.get());
    if (!state)
        return "rare scout without its state";
    char const* observed = state->observation == RareScoutObservation::Unloaded ? "unloaded" :
        (state->observation == RareScoutObservation::Dead ? "dead" : "alive");
    bool const retiring = state->retireOutcome != ActivityOutcome::None;
    uint64 const deadline = retiring ? state->retireUntilMs : state->waitUntilMs;
    uint64 const left = deadline > nowMs ? (deadline - nowMs) / 1000 : 0;
    return Acore::StringFormat("rare spawn {}: {}, confirmed {}, announced {}, {} {} s left", state->spawn.spawnId,
        observed, state->sightedAtMs.has_value(), state->announced, retiring ? "withdrawing" : "observing", left);
}

std::unique_ptr<ActivityKind> CreateRareScoutKind() { return std::make_unique<RareScoutKind>(); }
