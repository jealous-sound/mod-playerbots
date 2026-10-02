/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CityLoiter.h"
#include "ActivityBrain.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "ActivityOrganizer.h"
#include "CoaThread.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace
{
constexpr char const* LOG = "playerbots.activity";
constexpr float LOITER_GATHER_RADIUS = 20.0f;         // Gathering: arrived at the spot
constexpr float LOITER_LEASH = 25.0f;                 // Active: the crowd stays this close to the spot
constexpr float LOITER_NEAR_YARDS = 1000.0f;          // a real player this close keeps the crowd (and places it)
constexpr uint64 LOITER_EMPTY_MS = 10 * 60 * 1000;    // nobody of the team near for this long: the crowd leaves
constexpr float LOITER_LINE_YARDS = 15.0f;            // a real player this close to a mounted loiterer hears it
constexpr uint32 LOITER_MOUNT_LINES_MAX = 2;          // show-off lines per crowd
constexpr uint64 LOITER_MOUNT_LINE_GAP_MS = 5 * 60 * 1000;

std::vector<uint32> const CLOSING_EMOTES = {TEXT_EMOTE_WAVE, TEXT_EMOTE_BOW, TEXT_EMOTE_CHEER};

CityLoiterState* StateOf(Activity& activity) { return dynamic_cast<CityLoiterState*>(activity.state.get()); }

bool TeamFits(uint8 spotTeam, uint8 team) { return spotTeam == TEAM_NEUTRAL || spotTeam == team; }

// The player stands in that city (a capital zone) or within LOITER_NEAR_YARDS of the spot.
bool PlayerNearCity(Player* player, ActivityPoint const& spot)
{
    if (player->GetMapId() != spot.mapId)
        return false;
    if (spot.zoneId && player->GetZoneId() == spot.zoneId)
    {
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(spot.zoneId);
        if (zone && (zone->flags & AREA_FLAG_CAPITAL))
            return true;
    }
    return spot.SqDist2d(player->GetPositionX(), player->GetPositionY()) <= LOITER_NEAR_YARDS * LOITER_NEAR_YARDS;
}

// A real player of the team within LOITER_NEAR_YARDS of the spot.
bool TeamPlayerNear(std::vector<Player*> const& realPlayers, uint8 team, ActivityPoint const& spot)
{
    for (Player* player : realPlayers)
        if (player && player->IsInWorld() && player->GetTeamId() == team && player->GetMapId() == spot.mapId &&
            spot.SqDist2d(player->GetPositionX(), player->GetPositionY()) <= LOITER_NEAR_YARDS * LOITER_NEAR_YARDS)
            return true;
    return false;
}

// A mounted loiterer with a real player close by says its show-off line (at most LOITER_MOUNT_LINES_MAX, 5 min apart).
void ShowOff(ActivityServices& services, Activity& activity, CityLoiterState& state)
{
    if (state.mountLines >= LOITER_MOUNT_LINES_MAX || services.nowMs < state.nextMountLineMs)
        return;

    float const rangeSq = LOITER_LINE_YARDS * LOITER_LINE_YARDS;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || !member.isBot)
            continue;
        Player* bot = ActivityWorld::FindMember(member);
        if (!bot || !bot->IsMounted() || bot->IsInCombat())
            continue;
        bool const heard = std::any_of(services.realPlayers.begin(), services.realPlayers.end(),
                                       [bot, rangeSq](Player* player)
                                       {
                                           return player && player->IsInWorld() && player->GetMap() == bot->GetMap() &&
                                                  !player->IsGameMaster() &&
                                                  bot->GetExactDist2dSq(player) <= rangeSq;
                                       });
        if (!heard)
            continue;

        ActivityOpResult const result = services.ops.PostToChannel(
            member.guid, ActivityChatTarget::Say,
            ActivityWorld::Line("coa_act_loiter_mount", "Yes, that is my mount. No, you cannot borrow it."));
        if (result == ActivityOpResult::Deferred)
            return;
        if (result == ActivityOpResult::Done)
        {
            ++state.mountLines;
            state.nextMountLineMs = services.nowMs + LOITER_MOUNT_LINE_GAP_MS;
            LOG_DEBUG(LOG, "activity #{} loiter: show-off line {}", activity.id, state.mountLines);
        }
        return;
    }
}

void EnterActive(ActivityServices& services, Activity& activity, CityLoiterState& state)
{
    uint32 const minutes = services.config.loiterMinutes;
    uint32 const lo = uint32(std::max<int32>(5, int32(minutes) - 10));
    state.activeUntilMs = services.nowMs + uint64(urand(lo, minutes + 10)) * MINUTE * IN_MILLISECONDS;
    state.emptySinceMs = 0;
    activity.SetCenter(state.spot, LOITER_LEASH);
    activity.SetTravel(ActivityTravel::StayNearCenter);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
}

// The closing emote: the brain's pick when it answers within BrainDeadlineMs, else each bot's own; then CELEBRATE.
void Celebrate(ActivityServices& services, Activity& activity)
{
    if (activity.viewFlags & ACTIVITY_VIEW_CELEBRATE)
        return;
    ActivityAskState const asked = services.organizer.State(activity.id, ActivityAsk::ClosingEmote);
    if (asked == ActivityAskState::Pending)
        return;
    if (asked == ActivityAskState::Ready)
        if (std::optional<BrainResult> const result = services.organizer.Take(activity.id, ActivityAsk::ClosingEmote))
            if (std::optional<std::size_t> const index = ActivityBrain::ReadEmote(*result, CLOSING_EMOTES.size()))
                activity.SetClosingEmote(CLOSING_EMOTES[*index]);
    activity.SetFlags(ACTIVITY_VIEW_CELEBRATE);
}

void EnterResolving(ActivityServices& services, Activity& activity)
{
    activity.SetFlags(0);
    activity.SetTravel(ActivityTravel::None);
    activity.SetPhase(ActivityPhase::Resolving, services.nowMs);
    if (services.organizer.Usable())
        services.organizer.Ask(activity.id, ActivityAsk::ClosingEmote,
                               ActivityBrain::BuildEmoteJob(activity.kind, true, CLOSING_EMOTES, 0, 0));
    Celebrate(services, activity);
}
}  // namespace

CityLoiterKind::SpotCheck const& CityLoiterKind::Check(ActivitySpot const& spot)
{
    std::string const key = Acore::StringFormat("{}:{}:{}:{}:{}", spot.name, spot.mapId, spot.x, spot.y, spot.z);
    auto const known = spotChecks.find(key);
    if (known != spotChecks.end())
        return known->second;

    SpotCheck check;
    MapEntry const* entry = sMapStore.LookupEntry(spot.mapId);
    // CreateBaseMap asserts on an unknown map; an instanced map has no single base map to stand on.
    Map* map = entry && !entry->Instanceable() ? sMapMgr->CreateBaseMap(spot.mapId) : nullptr;
    float const ground = map ? map->GetHeight(PHASEMASK_NORMAL, spot.x, spot.y, spot.z + 10.0f) : INVALID_HEIGHT;
    if (map && ground > INVALID_HEIGHT)
    {
        check.good = true;
        check.point.mapId = spot.mapId;
        check.point.x = spot.x;
        check.point.y = spot.y;
        check.point.z = ground + 0.05f;
        check.point.zoneId = map->GetZoneId(PHASEMASK_NORMAL, spot.x, spot.y, ground);
        check.point.set = true;
        check.areaId = map->GetAreaId(PHASEMASK_NORMAL, spot.x, spot.y, ground);
        LOG_DEBUG(LOG, "loiter spot {} checked: area {}, zone {}, ground {}", spot.name, check.areaId,
                  check.point.zoneId, check.point.z);
    }
    else
        LOG_WARN(LOG, "CoA activity: loiter spot {} is not usable ({})", spot.name,
                 map ? "no ground at its coordinates" : "not an open world map");
    return spotChecks.emplace(key, check).first->second;
}

bool CityLoiterKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                          std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    ActivityConfig const& config = services.config;
    Player* const p = request.nearPlayer;
    if (!p && !(request.gm && !request.target.empty()))
    {
        why = "no real player to place it near";
        return false;
    }

    std::optional<uint8> team = request.team;
    if (!team && p)
        team = uint8(p->GetTeamId());

    ActivitySpot const* spot = nullptr;
    if (!request.target.empty())
    {
        spot = config.FindLoiterSpot(request.target);
        if (!spot || !Check(*spot).good)
        {
            why = "unknown or unusable loiter spot " + request.target;
            return false;
        }
        if (team && !TeamFits(spot->team, *team))
        {
            why = Acore::StringFormat("loiter spot {} is not for that faction", spot->name);
            return false;
        }
        if (!team)
            team = spot->team == TEAM_NEUTRAL ? uint8(urand(0, 1)) : spot->team;
        if (cityInUse.count(Check(*spot).point.zoneId))
        {
            why = "a crowd already loiters in that city";
            return false;
        }
    }
    else
    {
        // The nearest usable spot of the player's faction (or any) whose city the player is in or near.
        float bestSq = std::numeric_limits<float>::max();
        for (ActivitySpot const& candidate : config.loiterSpots)
        {
            if (candidate.mapId != p->GetMapId() || !TeamFits(candidate.team, *team))
                continue;
            float const distSq = (candidate.x - p->GetPositionX()) * (candidate.x - p->GetPositionX()) +
                                 (candidate.y - p->GetPositionY()) * (candidate.y - p->GetPositionY());
            if (distSq >= bestSq)
                continue;
            SpotCheck const& check = Check(candidate);
            if (!check.good || cityInUse.count(check.point.zoneId) || !PlayerNearCity(p, check.point))
                continue;
            bestSq = distSq;
            spot = &candidate;
        }
        if (!spot)
        {
            why = "no free city of that faction near the player";
            return false;
        }
    }

    SpotCheck const& check = Check(*spot);

    // Grown-ups show their mounts: Loiter.MinLevel and above, unless the GM names a level.
    uint8 minLevel = uint8(std::clamp<uint32>(config.loiterMinLevel, 1, 80));
    uint8 maxLevel = 80;
    if (request.level)
    {
        minLevel = uint8(std::clamp<int32>(int32(request.level) - 5, 1, 80));
        maxLevel = uint8(std::clamp<int32>(int32(request.level) + 5, 1, 80));
    }

    uint32 const cap = std::min(config.loiterMaxBots, ActivityThreshold::LOITER_MAX_BOTS);
    uint32 wanted = std::clamp(request.size ? request.size : cap, config.loiterMinBots,
                               std::max(config.loiterMinBots, cap));
    wanted = std::min({wanted, request.capacity, ActivityThreshold::LOITER_MAX_BOTS});
    uint32 const needed = config.loiterMinBots;
    if (wanted < needed)
    {
        why = Acore::StringFormat("only {} participant places left, a crowd needs {}", request.capacity, needed);
        return false;
    }

    ActivityCandidateQuery query;
    query.team = *team;
    query.minLevel = minLevel;
    query.maxLevel = maxLevel;
    query.center = check.point;
    query.limit = 2 * wanted;
    std::vector<ActivityCandidate> found = services.candidates.Find(query);
    if (found.size() < needed)
    {
        why = Acore::StringFormat("only {} free bots of that faction in levels {}-{}", found.size(), minLevel,
                                  maxLevel);
        return false;
    }

    auto state = std::make_unique<CityLoiterState>();
    state->spotName = spot->name;
    state->spot = check.point;
    state->areaId = check.areaId;

    std::string const city = ActivityWorld::AreaName(check.point.zoneId);
    plan.kind = ActivityKindId::Loiter;
    plan.gm = request.gm;
    plan.team = *team;
    plan.minLevel = minLevel;
    plan.maxLevel = maxLevel;
    plan.rally = check.point;
    plan.target = spot->name;
    plan.name = city.empty() ? spot->name : city;
    plan.wanted = wanted;
    plan.needed = needed;
    plan.leaderFixed = false;
    plan.candidates = std::move(found);
    plan.decisions.clear();
    plan.state = std::move(state);

    LOG_DEBUG(LOG, "loiter plan: {} (zone {}), levels {}-{}, {} candidates, wanted {}, needed {}", spot->name,
              check.point.zoneId, minLevel, maxLevel, plan.candidates.size(), wanted, needed);
    return true;
}

bool CityLoiterKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    CityLoiterState* state = dynamic_cast<CityLoiterState*>(plan.state.get());
    if (!state || !state->spot.IsSet() || cityInUse.count(state->spot.zoneId))
        return false;

    std::vector<ActivityCandidate const*> accepted;
    for (std::size_t i = 0; i < plan.candidates.size() && accepted.size() < plan.wanted; ++i)
        if (i < plan.decisions.size() && plan.decisions[i] == 1)
            accepted.push_back(&plan.candidates[i]);
    if (accepted.size() < plan.needed)
        return false;

    activity.name = plan.name;
    activity.target = plan.target;
    activity.minLevel = plan.minLevel;
    activity.maxLevel = plan.maxLevel;
    for (ActivityCandidate const* candidate : accepted)
        activity.Enroll(candidate->guid, candidate->epoch, true, candidate->level, services.nowUnix);

    uint64 const minutes = services.config.loiterMinutes + 10 + 10 + ActivityThreshold::HARD_DEADLINE_EXTRA_MIN;
    activity.hardDeadlineMs = services.nowMs + minutes * MINUTE * IN_MILLISECONDS;
    cityInUse[state->spot.zoneId] = activity.id;

    activity.SetStyle(ACTIVITY_STYLE_LOITER);
    activity.SetCenter(state->spot, LOITER_GATHER_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
    activity.state = std::move(plan.state);
    return true;
}

void CityLoiterKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;

    CityLoiterState* state = StateOf(activity);
    if (!state)
    {
        activity.Finish(ActivityOutcome::TooFew, "city crowd without its state");
        return;
    }
    if (activity.phase != ActivityPhase::Resolving && activity.BotsIn() < 2)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: fewer than 2 bots left", state->spotName));
        return;
    }

    uint64 const now = services.nowMs;
    switch (activity.phase)
    {
        case ActivityPhase::Gathering:
        {
            uint32 arrived = 0;
            for (ActivityMember const& member : activity.members)
                if (member.In() && member.isBot && member.arrived)
                    ++arrived;
            if (arrived >= std::min(services.config.loiterMinBots, activity.BotsIn()))
            {
                EnterActive(services, activity, *state);
                break;
            }
            if (now < activity.phaseSinceMs + uint64(services.config.travelGatherTimeoutSeconds) * IN_MILLISECONDS)
                break;

            // Time is up: the late ones still far away stay home.
            float const stragglerYards = float(services.config.travelStragglerYards);
            for (ActivityMember const& member : activity.members)
            {
                if (!member.In() || !member.isBot || member.arrived)
                    continue;
                Player* bot = ActivityWorld::FindMember(member);
                if (!bot || bot->GetMapId() != state->spot.mapId ||
                    state->spot.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) > stragglerYards * stragglerYards)
                    activity.MarkLeft(member.guid, ActivityLeaveReason::Dropped, services.nowUnix);
            }
            if (activity.BotsIn() < 2)
            {
                activity.Finish(ActivityOutcome::TooFew,
                                Acore::StringFormat("{}: too few came to the city", state->spotName));
                break;
            }
            EnterActive(services, activity, *state);
            break;
        }
        case ActivityPhase::Active:
        {
            // Nobody of the team around for LOITER_EMPTY_MS: the crowd disperses early.
            if (TeamPlayerNear(services.realPlayers, activity.team, state->spot))
                state->emptySinceMs = 0;
            else if (!state->emptySinceMs)
                state->emptySinceMs = now;

            ShowOff(services, activity, *state);
            if (now >= state->activeUntilMs || (state->emptySinceMs && now >= state->emptySinceMs + LOITER_EMPTY_MS))
                EnterResolving(services, activity);
            break;
        }
        case ActivityPhase::Resolving:
            Celebrate(services, activity);
            if (now >= activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS)
                activity.Finish(ActivityOutcome::Success,
                                Acore::StringFormat("{}: {} show-off lines{}", state->spotName, state->mountLines,
                                                    state->emptySinceMs ? ", nobody left around" : ""));
            break;
        default:
            break;
    }
}

void CityLoiterKind::OnMemberLeft(ActivityServices& /*services*/, Activity& activity,
                                  ActivityMember const& /*member*/)
{
    // Resolving ends by itself within RESOLVE_MS: it stays a success.
    if (activity.phase != ActivityPhase::Resolving && activity.BotsIn() < 2)
        activity.Finish(ActivityOutcome::TooFew, "fewer than 2 bots left");
}

std::string CityLoiterKind::Join(ActivityServices& /*services*/, Activity& activity, Player* /*player*/)
{
    COA_ASSERT_WORLD_THREAD();
    CityLoiterState const* state = StateOf(activity);
    return ActivityWorld::Line("coa_act_join_spot", "They are at %area (%zone): come by!",
                               {{"%area", ActivityWorld::AreaName(state ? state->areaId : 0)},
                                {"%zone", ActivityWorld::AreaName(state ? state->spot.zoneId : 0)}});
}

void CityLoiterKind::OnEnd(ActivityServices& /*services*/, Activity& activity)
{
    CityLoiterState const* state = StateOf(activity);
    if (!state)
        return;
    auto const it = cityInUse.find(state->spot.zoneId);
    if (it != cityInUse.end() && it->second == activity.id)
        cityInUse.erase(it);
}

std::string CityLoiterKind::Describe(Activity const& activity, uint64 nowMs) const
{
    CityLoiterState const* state = dynamic_cast<CityLoiterState const*>(activity.state.get());
    if (!state)
        return "city crowd without its state";

    uint64 endMs = activity.hardDeadlineMs;
    if (activity.phase == ActivityPhase::Active)
        endMs = state->activeUntilMs;
    else if (activity.phase == ActivityPhase::Resolving)
        endMs = activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS;
    uint64 const endsIn = endMs > nowMs ? (endMs - nowMs) / IN_MILLISECONDS : 0;
    uint64 const emptyFor = state->emptySinceMs && nowMs > state->emptySinceMs
                                ? (nowMs - state->emptySinceMs) / IN_MILLISECONDS
                                : 0;

    std::string const area = ActivityWorld::AreaName(state->areaId);
    return Acore::StringFormat("crowd at {} ({}), show-off lines {}/{}, nobody near for {} s, ends in {} s",
                               state->spotName, area.empty() ? "?" : area, state->mountLines, LOITER_MOUNT_LINES_MAX,
                               emptyFor, endsIn);
}

std::unique_ptr<ActivityKind> CreateCityLoiterKind() { return std::make_unique<CityLoiterKind>(); }
