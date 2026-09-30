/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GangParty.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "CoaThread.h"
#include "DBCStores.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "TravelMgr.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
constexpr char const* LOG = "playerbots.activity";
constexpr std::size_t LEADER_TRIES = 5;       // the bravest free bots tried as leader
constexpr uint32 LEADER_POOL = 40;            // free bots of the band looked at for the leader
constexpr float CREW_YARDS = 1500.0f;         // the crew is found this close to its leader
constexpr float ROUTE_REACH_YARDS = 6000.0f;  // grind spots farther from the leader are not looked at
constexpr std::size_t ROUTE_SAMPLES = 160;    // zone lookups per route at most, spread evenly by distance
constexpr float ROUTE_HOP_YARDS = 2500.0f;    // the next stop's zone has a spot this close to the previous stop
constexpr float STOP_SPREAD = 10.0f;          // a stop's center: this close to one of its zone's grind spots
constexpr float MARCH_RADIUS = 25.0f;         // Marching: the leader reached the stop
constexpr float SPOT_PROBE_ABOVE = 30.0f;     // the grind cache rounds z to 50 yards: look for the ground from
constexpr float SPOT_PROBE_DEPTH = 80.0f;     // this high above, this deep
constexpr float WATER_PROBE_HEIGHT = 2.0f;    // a standing character's collision height, for the water test
constexpr uint32 LINE_RETRY_MS = 15000;       // a leader's line is retried this long (budget, combat)

char const* TeamName(uint8 team) { return team == TEAM_ALLIANCE ? "alliance" : "horde"; }

// A spoken line may start with a placeholder (the gang's name): the first ASCII letter goes upper case.
std::string Spoken(std::string text)
{
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
        text[0] = char(text[0] - 'a' + 'A');
    return text;
}

GangPartyState* StateOf(Activity& activity) { return dynamic_cast<GangPartyState*>(activity.state.get()); }

ActivityPoint CandidatePoint(ActivityCandidate const& candidate)
{
    ActivityPoint point;
    point.mapId = candidate.mapId;
    point.x = candidate.x;
    point.y = candidate.y;
    point.z = candidate.z;
    point.zoneId = candidate.zoneId;
    point.set = true;
    return point;
}

std::string ZoneName(GangStop const& stop) { return ActivityWorld::AreaName(stop.center.zoneId); }

// The grind spots of one zone around the leader.
struct ZoneSpots
{
    uint32 zoneId = 0;
    float nearest = std::numeric_limits<float>::max();  // yards from the leader to its nearest spot
    std::vector<ActivityPoint> spots;
};

// A stop's center: on the ground near a random grind spot of the zone (the others when one has no ground).
ActivityPoint StopCenter(ZoneSpots const& zone)
{
    std::vector<std::size_t> order(zone.spots.size());
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::shuffle(order.begin(), order.end(), RandomEngine::Instance());
    for (std::size_t const index : order)
    {
        ActivityPoint center = ActivityWorld::GroundNear(zone.spots[index], STOP_SPREAD);
        if (center.IsSet())
        {
            center.zoneId = zone.zoneId;
            return center;
        }
    }
    return {};
}

// 2-3 hunting stops for the leader's level on its map (contract section 13): the grind spots of
// sTravelMgr's level cache, outside enemy zones, capitals and water, grouped by zone (two spots at least); the
// first stop in the zone of the spot nearest the leader, each next one in the nearest unused zone with a spot within
// ROUTE_HOP_YARDS of the previous stop. World thread: the map lookups run while the map threads are idle.
bool BuildRoute(ActivityCandidate const& leader, uint32 stopsWanted, std::vector<GangStop>& route, std::string& why)
{
    route.clear();
    why = Acore::StringFormat("no hunting ground for level {} on this map", leader.level);
    Map* map = sMapMgr->FindBaseNonInstanceMap(leader.mapId);
    if (!map)
        return false;

    std::vector<std::pair<float, WorldLocation const*>> nearSpots;
    for (WorldLocation const& loc : sTravelMgr.GetLocsPerLevelCache(leader.level))
    {
        if (loc.GetMapId() != leader.mapId)
            continue;
        float const dx = loc.GetPositionX() - leader.x;
        float const dy = loc.GetPositionY() - leader.y;
        float const dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= ROUTE_REACH_YARDS)
            nearSpots.emplace_back(dist, &loc);
    }
    if (nearSpots.empty())
        return false;
    std::sort(nearSpots.begin(), nearSpots.end(),
              [](std::pair<float, WorldLocation const*> const& a, std::pair<float, WorldLocation const*> const& b)
              { return a.first < b.first; });

    std::vector<ZoneSpots> zones;
    std::unordered_map<uint32, std::size_t> zoneIndex;
    std::size_t const samples = std::min(nearSpots.size(), ROUTE_SAMPLES);
    for (std::size_t k = 0; k < samples; ++k)
    {
        auto const& [dist, loc] = nearSpots[k * nearSpots.size() / samples];
        float const x = loc->GetPositionX();
        float const y = loc->GetPositionY();
        float const ground =
            map->GetHeight(PHASEMASK_NORMAL, x, y, loc->GetPositionZ() + SPOT_PROBE_ABOVE, true, SPOT_PROBE_DEPTH);
        if (ground <= INVALID_HEIGHT)
            continue;
        uint32 const zoneId = map->GetZoneId(PHASEMASK_NORMAL, x, y, ground);
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);
        if (!zone || (zone->flags & AREA_FLAG_CAPITAL))
            continue;
        // Enemy territory, as RandomTeleport.
        if ((zone->team == AREATEAM_HORDE && leader.team == TEAM_ALLIANCE) ||
            (zone->team == AREATEAM_ALLY && leader.team == TEAM_HORDE))
            continue;
        if (map->IsInWater(PHASEMASK_NORMAL, x, y, ground, WATER_PROBE_HEIGHT))
            continue;

        auto const [slot, fresh] = zoneIndex.emplace(zoneId, zones.size());
        if (fresh)
        {
            zones.emplace_back();
            zones.back().zoneId = zoneId;
        }
        ZoneSpots& spots = zones[slot->second];
        ActivityPoint point;
        point.mapId = leader.mapId;
        point.x = x;
        point.y = y;
        point.z = ground + 0.05f;
        point.zoneId = zoneId;
        point.set = true;
        spots.spots.push_back(point);
        spots.nearest = std::min(spots.nearest, dist);
    }
    zones.erase(std::remove_if(zones.begin(), zones.end(), [](ZoneSpots const& zone) { return zone.spots.size() < 2; }),
                zones.end());

    std::vector<bool> used(zones.size(), false);
    while (route.size() < stopsWanted)
    {
        std::size_t best = zones.size();
        float bestDist = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < zones.size(); ++i)
        {
            if (used[i])
                continue;
            float dist = zones[i].nearest;
            if (!route.empty())
            {
                ActivityPoint const& previous = route.back().center;
                dist = std::numeric_limits<float>::max();
                for (ActivityPoint const& spot : zones[i].spots)
                    dist = std::min(dist, std::sqrt(previous.SqDist2d(spot.x, spot.y)));
                if (dist > ROUTE_HOP_YARDS)
                    continue;
            }
            if (dist < bestDist)
            {
                best = i;
                bestDist = dist;
            }
        }
        if (best == zones.size())
            break;
        used[best] = true;
        ActivityPoint const center = StopCenter(zones[best]);
        if (center.IsSet())
            route.push_back({center, 0});
    }
    return !route.empty();
}

void DropLine(GangPartyState& state)
{
    if (state.lineDue == GangLine::Form)
        state.announced = true;
    state.lineDue = GangLine::None;
}

void LineDue(GangPartyState& state, GangLine line, uint64 nowMs)
{
    DropLine(state);
    state.lineDue = line;
    state.lineDueUntilMs = nowMs + LINE_RETRY_MS;
}

// The leader says the due line; the budget or a fight delays it for LINE_RETRY_MS at most.
void SayDue(ActivityServices& services, Activity& activity, GangPartyState& state)
{
    if (state.lineDue == GangLine::None)
        return;
    if (services.nowMs >= state.lineDueUntilMs)
    {
        LOG_DEBUG(LOG, "activity #{} gang: line {} given up", activity.id, uint32(state.lineDue));
        DropLine(state);
        return;
    }

    std::string text;
    switch (state.lineDue)
    {
        case GangLine::Form:
            text = ActivityWorld::Line("coa_act_gang_form", "%gang, with me! We hunt in %zone.",
                                       {{"%gang", activity.name}, {"%zone", ZoneName(state.route.front())}});
            break;
        case GangLine::Move:
            text = ActivityWorld::Line("coa_act_gang_move", "On to %zone!",
                                       {{"%zone", ZoneName(state.route[state.stop])}});
            break;
        case GangLine::Brag:
        {
            uint64 const minutes = services.nowMs > activity.startedMs
                                       ? (services.nowMs - activity.startedMs) / MINUTE / IN_MILLISECONDS
                                       : 0;
            text = ActivityWorld::Line("coa_act_gang_brag", "Good hunt, %gang. Nobody stood in our way.",
                                       {{"%gang", activity.name},
                                        {"%zone", ZoneName(state.route.back())},
                                        {"%minutes", std::to_string(minutes)}});
            break;
        }
        default:
            break;
    }

    ActivityOpResult const result =
        services.ops.PostToChannel(activity.leader, ActivityChatTarget::Say, Spoken(std::move(text)));
    if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
        return;
    DropLine(state);
}

void EnterGathering(ActivityServices& services, Activity& activity, GangPartyState& state, Player* leader)
{
    state.gatherUntilMs = services.nowMs + uint64(services.config.travelGatherTimeoutSeconds) * IN_MILLISECONDS;
    activity.SetCenter(ActivityWorld::PointOf(leader), ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetFlags(ACTIVITY_VIEW_HOLD);
    activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
    if (!state.announced)
        LineDue(state, GangLine::Form, services.nowMs);
}

void EnterMarching(ActivityServices& services, Activity& activity, GangPartyState& state)
{
    activity.SetCenter(state.route[state.stop].center, MARCH_RADIUS);
    activity.SetTravel(ActivityTravel::FollowLeader);
    activity.SetFlags(0);  // the director's regroup sets HOLD while Marching
    activity.SetPhase(ActivityPhase::Marching, services.nowMs);
    LineDue(state, GangLine::Move, services.nowMs);
}

void EnterActive(ActivityServices& services, Activity& activity, GangPartyState& state)
{
    GangStop const& stop = state.route[state.stop];
    state.huntUntilMs = services.nowMs + stop.huntMs;
    activity.SetCenter(stop.center, ActivityThreshold::HUNT_RADIUS);
    activity.SetTravel(ActivityTravel::FollowLeader);
    activity.SetFlags(ACTIVITY_VIEW_HUNT);
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
}

void EnterResolving(ActivityServices& services, Activity& activity, GangPartyState& state)
{
    activity.SetFlags(ACTIVITY_VIEW_CELEBRATE);
    activity.SetTravel(ActivityTravel::None);
    activity.SetPhase(ActivityPhase::Resolving, services.nowMs);
    LineDue(state, GangLine::Brag, services.nowMs);
}

// Forming: the leader's group with every member at once when the budget allows it.
void Form(ActivityServices& services, Activity& activity, GangPartyState& state, Player* leader)
{
    if (services.nowMs >= state.formSinceMs + ActivityThreshold::FORM_TIMEOUT_MS)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: no group in time", activity.name));
        return;
    }

    std::vector<ObjectGuid> members;
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot && member.guid != activity.leader)
            members.push_back(member.guid);

    // The director's member pass drops a member without the activity's group once the group exists, before
    // AddMember could add it on a later tick: wait for a tick whose budget adds everyone (when MaxOpsPerTick can).
    uint32 const cost = 1 + uint32(members.size());
    uint32 const budget = services.config.maxOpsPerTick;
    if (cost <= budget && services.ops.Used() + cost > budget)
        return;

    ObjectGuid groupGuid;
    std::vector<ObjectGuid> added;
    std::vector<ObjectGuid> skipped;
    ActivityOpResult const result = services.ops.FormGroup(activity.leader, members, uint8(GROUP_LOOT),
                                                           uint8(ITEM_QUALITY_UNCOMMON), groupGuid, added, skipped);
    if (result == ActivityOpResult::Deferred)
        return;
    if (result != ActivityOpResult::Done || groupGuid.IsEmpty())
    {
        activity.Finish(ActivityOutcome::TooFew,
                        Acore::StringFormat("{}: group not formed ({})", activity.name, ActivityOpResultName(result)));
        return;
    }

    activity.groupGuid = groupGuid;
    activity.viewDirty = true;
    state.toAdd = std::move(skipped);
    LOG_DEBUG(LOG, "activity #{} gang: group {} formed, {} added, {} left for later", activity.id,
              groupGuid.GetCounter(), added.size(), state.toAdd.size());
    EnterGathering(services, activity, state, leader);
}

// Gathering: the bots come to the leader; a real player who just joined is waited for (RallyWaitSeconds).
void Gather(ActivityServices& services, Activity& activity, GangPartyState& state, Player* leader)
{
    ActivityConfig const& config = services.config;
    float const radius = ActivityThreshold::RALLY_ARRIVE_RADIUS;

    uint32 arrived = 0;
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot && member.arrived)
            ++arrived;

    bool waitForPlayer = false;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || member.isBot || services.nowUnix >= member.joinedUnix + config.travelRallyWaitSeconds)
            continue;
        Player* player = ActivityWorld::FindMember(member);
        if (player && (player->GetMapId() != activity.center.mapId ||
                       activity.center.SqDist2d(player->GetPositionX(), player->GetPositionY()) > radius * radius))
        {
            waitForPlayer = true;
            break;
        }
    }

    if (arrived * 100 >= activity.BotsIn() * ActivityThreshold::REGROUP_PERCENT && !waitForPlayer)
    {
        EnterMarching(services, activity, state);
        return;
    }
    if (services.nowMs < state.gatherUntilMs)
        return;

    // Time is up: bots still far from the leader stay behind.
    float const stragglerYards = float(config.travelStragglerYards);
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || !member.isBot || member.arrived || member.guid == activity.leader)
            continue;
        Player* bot = ActivityWorld::FindMember(member);
        if (!bot || bot->GetMapId() != leader->GetMapId() || bot->GetExactDist2d(leader) > stragglerYards)
            activity.MarkLeft(member.guid, ActivityLeaveReason::Dropped, services.nowUnix);
    }
    if (activity.BotsIn() < 2 && activity.PlayersIn() == 0)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: too few came to the rally", activity.name));
        return;
    }
    EnterMarching(services, activity, state);
}

// Bots FormGroup left out for the budget (only when MaxOpsPerTick is below the gang's size).
void AddLate(ActivityServices& services, Activity& activity, GangPartyState& state)
{
    if (state.toAdd.empty() || activity.groupGuid.IsEmpty() || activity.phase == ActivityPhase::Resolving)
        return;
    for (auto it = state.toAdd.begin(); it != state.toAdd.end();)
    {
        ActivityMember const* member = activity.FindMember(*it);
        if (!member || !member->In())
        {
            it = state.toAdd.erase(it);
            continue;
        }
        ActivityOpResult const result = services.ops.AddMember(activity.groupGuid, activity.leader, *it);
        if (result == ActivityOpResult::Deferred)
            break;
        if (result != ActivityOpResult::Done)
            activity.MarkLeft(*it, ActivityLeaveReason::Dropped, services.nowUnix);
        it = state.toAdd.erase(it);
    }
}
}  // namespace

bool GangPartyKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                         std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    ActivityConfig const& config = services.config;
    Player* const nearPlayer = request.nearPlayer;

    uint8 const team =
        request.team ? *request.team : (nearPlayer ? uint8(nearPlayer->GetTeamId()) : uint8(urand(0, 1)));

    // The level band: forced, else a bracket around the player's (random without one).
    uint8 minLevel = request.level;
    uint8 maxLevel = request.level;
    if (!request.level)
    {
        int32 const spread = int32(config.ambientNearPlayerBrackets);
        int32 bracket = nearPlayer ? int32(ActivityBracket(uint8(nearPlayer->GetLevel()))) + irand(-spread, spread)
                                   : int32(urand(0, ActivityThreshold::LEVEL_BANDS - 1));
        bracket = std::clamp<int32>(bracket, 0, int32(ActivityThreshold::LEVEL_BANDS) - 1);
        minLevel = uint8(10 * bracket + 1);
        maxLevel = uint8(10 * bracket + 10);
    }

    uint32 wanted = std::clamp(request.size ? request.size : config.gangMaxBots, config.gangMinBots,
                               std::max(config.gangMinBots, config.gangMaxBots));
    wanted = std::min(wanted, request.capacity);
    uint32 const needed = config.gangMinBots;
    if (wanted < needed)
    {
        why = Acore::StringFormat("only {} participant places left, a gang needs {}", request.capacity, needed);
        return false;
    }

    ActivityCandidateQuery first;
    first.team = team;
    first.minLevel = minLevel;
    first.maxLevel = maxLevel;
    if (nearPlayer)
        first.center = ActivityWorld::PointOf(nearPlayer);
    first.sameMapOnly = nearPlayer != nullptr;
    first.limit = LEADER_POOL;
    first.rulesetSafe = config.pvpWarMode;
    std::vector<ActivityCandidate> pool = services.candidates.Find(first);
    std::stable_sort(pool.begin(), pool.end(),
                     [](ActivityCandidate const& a, ActivityCandidate const& b) { return a.bravery > b.bravery; });

    // The bravest with enough free bots of its level around it leads; its route must exist.
    std::string routeWhy;
    for (std::size_t i = 0; i < pool.size() && i < LEADER_TRIES; ++i)
    {
        ActivityCandidate const& leader = pool[i];
        int32 const spread = int32(config.gangLevelSpread);
        std::unordered_set<uint32> const exclude{leader.guid.GetCounter()};
        ActivityCandidateQuery crewQuery;
        crewQuery.team = team;
        crewQuery.minLevel = uint8(std::clamp<int32>(int32(leader.level) - spread, 1, 255));
        crewQuery.maxLevel = uint8(std::clamp<int32>(int32(leader.level) + spread, 1, 255));
        crewQuery.center = CandidatePoint(leader);
        crewQuery.sameMapOnly = true;
        crewQuery.maxDistance = CREW_YARDS;
        crewQuery.limit = 2 * wanted - 1;
        crewQuery.rulesetSafe = config.pvpWarMode;
        crewQuery.exclude = &exclude;
        std::vector<ActivityCandidate> crew = services.candidates.Find(crewQuery);
        if (crew.size() + 1 < needed)
            continue;

        auto state = std::make_unique<GangPartyState>();
        if (!BuildRoute(leader, urand(2, 3), state->route, routeWhy))
            continue;

        uint32 const base = config.gangMinutes;
        state->minutes = urand(uint32(std::max<int32>(10, int32(base) - 15)), base + 15);
        uint32 const huntMs = uint32(uint64(state->minutes) * MINUTE * IN_MILLISECONDS * 3 / 4 / state->route.size());
        std::string target;
        for (GangStop& stop : state->route)
        {
            stop.huntMs = huntMs;
            target += (target.empty() ? "" : ",") + std::to_string(stop.center.zoneId);
        }

        plan.candidates.clear();
        plan.candidates.push_back(leader);
        plan.candidates.insert(plan.candidates.end(), crew.begin(), crew.end());
        uint8 lowest = leader.level;
        uint8 highest = leader.level;
        for (ActivityCandidate const& candidate : plan.candidates)
        {
            lowest = std::min(lowest, candidate.level);
            highest = std::max(highest, candidate.level);
        }

        plan.kind = ActivityKindId::Gang;
        plan.gm = request.gm;
        plan.team = team;
        plan.minLevel = lowest;
        plan.maxLevel = highest;
        plan.rally = CandidatePoint(leader);
        plan.target = target;
        plan.name = ActivityWorld::Line("coa_act_gang_name", "the Red Boars");
        plan.wanted = wanted;
        plan.needed = needed;
        plan.leaderFixed = true;
        plan.decisions.clear();

        LOG_DEBUG(LOG, "gang plan: leader {} level {} ({}), {} candidates, {} stops ({}), {} min, wanted {}",
                  leader.guid.GetCounter(), leader.level, TeamName(team), plan.candidates.size(), state->route.size(),
                  target, state->minutes, wanted);
        plan.state = std::move(state);
        return true;
    }

    why = !routeWhy.empty() ? routeWhy
                            : Acore::StringFormat("no {} bot of levels {}-{} with {} free bots near it", TeamName(team),
                                                  minLevel, maxLevel, needed - 1);
    return false;
}

bool GangPartyKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    GangPartyState* state = dynamic_cast<GangPartyState*>(plan.state.get());
    if (!state || state->route.empty() || plan.candidates.empty() || plan.decisions.empty() || plan.decisions[0] != 1)
        return false;

    std::vector<ActivityCandidate const*> accepted{&plan.candidates[0]};
    for (std::size_t i = 1; i < plan.candidates.size() && accepted.size() < plan.wanted; ++i)
        if (i < plan.decisions.size() && plan.decisions[i] == 1)
            accepted.push_back(&plan.candidates[i]);
    if (accepted.size() < plan.needed)
        return false;

    activity.leader = accepted.front()->guid;
    activity.name = plan.name;
    activity.target = plan.target;
    activity.warMode = services.config.pvpWarMode;
    uint8 lowest = accepted.front()->level;
    uint8 highest = lowest;
    for (ActivityCandidate const* candidate : accepted)
    {
        activity.Enroll(candidate->guid, candidate->epoch, true, candidate->level, services.nowUnix);
        lowest = std::min(lowest, candidate->level);
        highest = std::max(highest, candidate->level);
    }
    activity.minLevel = lowest;
    activity.maxLevel = highest;
    // The gathering may use its whole timeout on top of the planned minutes.
    activity.hardDeadlineMs =
        services.nowMs + uint64(services.config.travelGatherTimeoutSeconds) * IN_MILLISECONDS +
        uint64(state->minutes + ActivityThreshold::HARD_DEADLINE_EXTRA_MIN) * MINUTE * IN_MILLISECONDS;
    state->formSinceMs = services.nowMs;

    activity.state = std::move(plan.state);
    activity.SetCenter(plan.rally, ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_HOLD);
    activity.SetPhase(ActivityPhase::Forming, services.nowMs);
    return true;
}

void GangPartyKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;

    GangPartyState* state = StateOf(activity);
    if (!state || state->route.empty())
    {
        activity.Finish(ActivityOutcome::TooFew, "gang without a route");
        return;
    }
    if (activity.phase != ActivityPhase::Resolving && activity.BotsIn() < 2 && activity.PlayersIn() == 0)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: fewer than 2 bots left", activity.name));
        return;
    }

    uint64 const now = services.nowMs;
    // Resolving ends within RESOLVE_MS whoever is left: OnMemberLeft ends nothing then, not even for the leader.
    if (activity.phase == ActivityPhase::Resolving && now >= activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS)
    {
        uint64 const minutes = now > activity.startedMs ? (now - activity.startedMs) / MINUTE / IN_MILLISECONDS : 0;
        activity.Finish(ActivityOutcome::Success, Acore::StringFormat("{}: {} stops ({}), {} min", activity.name,
                                                                      state->route.size(), activity.target, minutes));
        return;
    }

    // A leader not found was marked gone by the director's pass just before: OnMemberLeft ended the gang (in
    // Resolving, the timeout above does).
    ActivityMember* leaderMember = activity.Leader();
    Player* leader = leaderMember && leaderMember->In() ? ActivityWorld::FindMember(*leaderMember) : nullptr;
    if (!leader)
        return;

    switch (activity.phase)
    {
        case ActivityPhase::Forming:
            Form(services, activity, *state, leader);
            break;
        case ActivityPhase::Gathering:
            Gather(services, activity, *state, leader);
            break;
        case ActivityPhase::Marching:
            if (leader->GetMapId() == activity.center.mapId &&
                activity.center.SqDist2d(leader->GetPositionX(), leader->GetPositionY()) <= MARCH_RADIUS * MARCH_RADIUS)
                EnterActive(services, activity, *state);
            break;
        case ActivityPhase::Active:
            if (now < state->huntUntilMs)
                break;
            if (state->stop + 1 < state->route.size())
            {
                ++state->stop;
                EnterMarching(services, activity, *state);
            }
            else
                EnterResolving(services, activity, *state);
            break;
        default:  // Resolving: its timeout is above
            break;
    }

    if (activity.finishing)
        return;
    AddLate(services, activity, *state);
    SayDue(services, activity, *state);
}

void GangPartyKind::OnMemberLeft(ActivityServices& /*services*/, Activity& activity, ActivityMember const& member)
{
    if (GangPartyState* state = StateOf(activity))
        state->toAdd.erase(std::remove(state->toAdd.begin(), state->toAdd.end(), member.guid), state->toAdd.end());

    // Resolving ends by itself within RESOLVE_MS: it stays a success.
    if (activity.phase == ActivityPhase::Resolving)
        return;
    if (member.guid == activity.leader)
    {
        activity.Finish(ActivityOutcome::LeaderGone, Acore::StringFormat("{}: the leader left ({})", activity.name,
                                                                         ActivityLeaveReasonName(member.leftReason)));
        return;
    }
    if (activity.BotsIn() < 2 && activity.PlayersIn() == 0)
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: fewer than 2 bots left", activity.name));
}

std::string GangPartyKind::Join(ActivityServices& services, Activity& activity, Player* player)
{
    COA_ASSERT_WORLD_THREAD();
    std::string const none = ActivityWorld::Line("coa_act_join_none", "No bot activity you can join right now.");
    if (!player || activity.phase == ActivityPhase::Resolving || activity.groupGuid.IsEmpty())
        return none;
    ActivityMember* leaderMember = activity.Leader();
    Player* leader = leaderMember && leaderMember->In() ? ActivityWorld::FindMember(*leaderMember) : nullptr;
    if (!leader)
        return none;

    ActivityOpResult const result = services.ops.InvitePlayer(activity.leader, player->GetGUID());
    LOG_DEBUG(LOG, "activity #{} gang: invite of {} {}", activity.id, player->GetGUID().GetCounter(),
              ActivityOpResultName(result));
    if (result == ActivityOpResult::Refused)
        return ActivityWorld::Line("coa_act_join_grouped", "Leave your group first.");
    if (result != ActivityOpResult::Done)
        return none;

    activity.invites[player->GetGUID()] = services.nowMs + ActivityThreshold::INVITE_TIMEOUT_MS;
    return ActivityWorld::Line("coa_act_join_invited", "%leader invites you to %gang: accept the group invitation.",
                               {{"%leader", ActivityWorld::FirstName(leader)}, {"%gang", activity.name}});
}

void GangPartyKind::OnEnd(ActivityServices& /*services*/, Activity& activity)
{
    GangPartyState* state = StateOf(activity);
    if (!state)
        return;
    LOG_DEBUG(LOG, "activity #{} gang: ends at stop {}/{}", activity.id, state->stop + 1, state->route.size());
    state->toAdd.clear();
    state->lineDue = GangLine::None;
}

std::string GangPartyKind::Describe(Activity const& activity, uint64 nowMs) const
{
    GangPartyState const* state = dynamic_cast<GangPartyState const*>(activity.state.get());
    if (!state || state->route.empty())
        return "gang without a route";

    std::string zones;
    for (GangStop const& stop : state->route)
    {
        std::string const name = ZoneName(stop);
        zones += (zones.empty() ? "" : ", ") + (name.empty() ? std::to_string(stop.center.zoneId) : name);
    }

    // Hunting time left: the current stop's (running or not yet begun) and the later stops'.
    uint64 huntLeftMs = 0;
    if (activity.phase != ActivityPhase::Resolving)
    {
        for (std::size_t i = state->stop; i < state->route.size(); ++i)
            huntLeftMs += state->route[i].huntMs;
        if (activity.phase == ActivityPhase::Active)
        {
            huntLeftMs -= state->route[state->stop].huntMs;
            huntLeftMs += state->huntUntilMs > nowMs ? state->huntUntilMs - nowMs : 0;
        }
    }

    return Acore::StringFormat("gang {}: stop {}/{} ({}), hunt left {} s, route {}", activity.name, state->stop + 1,
                               state->route.size(), zones, huntLeftMs / IN_MILLISECONDS, activity.target);
}

std::unique_ptr<ActivityKind> CreateGangPartyKind() { return std::make_unique<GangPartyKind>(); }
