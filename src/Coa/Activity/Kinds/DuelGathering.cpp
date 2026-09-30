/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DuelGathering.h"
#include "ActivityConfig.h"
#include "ActivityDuelCooldowns.h"
#include "ActivityOperations.h"
#include "CoaThread.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Player.h"
#include "Random.h"
#include "StringFormat.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

namespace
{
constexpr char const* LOG = "playerbots.activity";
constexpr int32 LEVEL_WINDOW = 3;        // ring levels: L +- this
constexpr uint32 MAX_SPEAKER_TRIES = 3;  // ring bots tried for one line or post
constexpr uint32 LINE_RETRY_MS = 10000;  // lines are retried this long after the phase began (budget, a fight)
constexpr uint32 ANY_LEVEL_POOL = 64;    // free bots looked at to pick a ring level when nobody sets one

char const* TeamName(uint8 team) { return team == TEAM_ALLIANCE ? "alliance" : "horde"; }

uint8 ClampLevel(int32 level, uint8 lo, uint8 hi)
{
    if (hi < lo)
        return lo;
    return uint8(std::clamp<int32>(level, lo, hi));
}

// A spoken line may start with a placeholder or a lower-case text row: the first ASCII letter goes upper case.
std::string Spoken(std::string text)
{
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
        text[0] = char(text[0] - 'a' + 'A');
    return text;
}

DuelGatheringState* StateOf(Activity& activity) { return dynamic_cast<DuelGatheringState*>(activity.state.get()); }

// Ring bots still in and in the world, in random order, those standing at the ring first (a /say is heard around
// the speaker only). `zoneId` != 0 keeps only bots in that zone (the zone channel).
std::vector<ObjectGuid> RingSpeakers(Activity const& activity, ObjectGuid skip, uint32 zoneId = 0)
{
    std::vector<ObjectGuid> close;
    std::vector<ObjectGuid> farAway;
    float const range = ActivityThreshold::RING_ARRIVE_RADIUS;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || !member.isBot || member.guid == skip)
            continue;
        Player* bot = ActivityWorld::FindMember(member);
        if (!bot || (zoneId && bot->GetZoneId() != zoneId))
            continue;
        bool const atRing = bot->GetMapId() == activity.center.mapId &&
                            activity.center.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) <= range * range;
        (atRing ? close : farAway).push_back(member.guid);
    }
    std::shuffle(close.begin(), close.end(), RandomEngine::Instance());
    std::shuffle(farAway.begin(), farAway.end(), RandomEngine::Instance());
    close.insert(close.end(), farAway.begin(), farAway.end());
    return close;
}

// Says `text` through the first speakers that can. Deferred when the budget ran out before anyone said it.
ActivityOpResult SayByOne(ActivityServices& services, std::vector<ObjectGuid> const& speakers,
                          ActivityChatTarget target, std::string const& text)
{
    ActivityOpResult last = ActivityOpResult::Gone;
    uint32 tries = 0;
    for (ObjectGuid const& speaker : speakers)
    {
        if (tries++ >= MAX_SPEAKER_TRIES)
            break;
        last = services.ops.PostToChannel(speaker, target, text);
        if (last == ActivityOpResult::Done || last == ActivityOpResult::Deferred)
            return last;
        // Refused (in combat), Failed (not in that channel), Gone: the next one.
    }
    return last;
}

// Deferred (budget) and Refused (every speaker fights) are tried again on the next ticks, up to LINE_RETRY_MS into
// the phase.
bool TryAgain(ActivityServices const& services, Activity const& activity, ActivityOpResult result)
{
    return (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused) &&
           services.nowMs < activity.phaseSinceMs + LINE_RETRY_MS;
}

// The opening (entering Active): a ring bot says the opening line, and when a real player is in the spot's zone a
// ring bot announces the ring in Chat.Channel.
void Open(ActivityServices& services, Activity& activity, DuelGatheringState& state)
{
    std::string const area = ActivityWorld::AreaName(state.spotAreaId);
    if (!state.opened)
    {
        std::string const line =
            Spoken(ActivityWorld::Line("coa_act_duel_open", "Right, who wants a duel?", {{"%area", area}}));
        if (TryAgain(services, activity,
                     SayByOne(services, RingSpeakers(activity, ObjectGuid::Empty), ActivityChatTarget::Say, line)))
            return;
        state.opened = true;
    }

    if (state.posted)
        return;

    ActivityConfig const& config = services.config;
    uint32 const zoneId = activity.center.zoneId;
    bool const playerInZone =
        zoneId && std::any_of(services.realPlayers.begin(), services.realPlayers.end(),
                              [zoneId](Player* player) { return player && player->GetZoneId() == zoneId; });
    if (config.chatChannel == ActivityChatChannel::None || !playerInZone || !activity.CanPost(config, services.nowMs))
    {
        state.posted = true;
        return;
    }

    bool const zone = config.chatChannel == ActivityChatChannel::Zone;
    std::string const line =
        Spoken(ActivityWorld::Line("coa_act_duel_lfm", "Duels at %area, come and test your skills!",
                                   {{"%area", area}, {"%zone", ActivityWorld::AreaName(zoneId)}}));
    ActivityOpResult const result = SayByOne(services, RingSpeakers(activity, ObjectGuid::Empty, zone ? zoneId : 0),
                                             zone ? ActivityChatTarget::Zone : ActivityChatTarget::World, line);
    if (TryAgain(services, activity, result))
        return;
    if (result == ActivityOpResult::Done)
        activity.NotePost(services.nowMs);
    state.posted = true;
}

// The closing (entering Resolving): the best duelist names its wins, another ring bot says goodbye.
void Close(ActivityServices& services, Activity& activity, DuelGatheringState& state)
{
    ActivityMember const* champion = nullptr;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || !member.isBot || member.duelsWon == 0)
            continue;
        if (!champion || member.duelsWon > champion->duelsWon ||
            (member.duelsWon == champion->duelsWon && member.guid.GetCounter() < champion->guid.GetCounter()))
            champion = &member;
    }
    ObjectGuid const championGuid = champion ? champion->guid : ObjectGuid::Empty;

    if (!state.championDone)
    {
        if (champion)
        {
            std::string const line =
                Spoken(ActivityWorld::Line("coa_act_duel_champion", "%wins wins today. Who is next?",
                                           {{"%wins", std::to_string(champion->duelsWon)}}));
            if (TryAgain(services, activity, services.ops.PostToChannel(championGuid, ActivityChatTarget::Say, line)))
                return;
        }
        state.championDone = true;
    }

    std::string const line =
        Spoken(ActivityWorld::Line("coa_act_duel_close", "That is enough for today. See you next time!"));
    if (TryAgain(services, activity,
                 SayByOne(services, RingSpeakers(activity, championGuid), ActivityChatTarget::Say, line)))
        return;
    state.closed = true;
}

void EnterActive(ActivityServices& services, Activity& activity, DuelGatheringState& state)
{
    uint32 const minutes = services.config.duelMinutes;
    uint32 const lo = uint32(std::max<int32>(5, int32(minutes) - 10));
    state.activeUntilMs = services.nowMs + uint64(urand(lo, minutes + 10)) * MINUTE * IN_MILLISECONDS;
    activity.SetCenter(activity.center, ActivityThreshold::RING_LEASH_RADIUS);
    activity.SetTravel(ActivityTravel::StayNearCenter);
    activity.SetFlags(ACTIVITY_VIEW_MAY_CHALLENGE | ACTIVITY_VIEW_MAY_CHALLENGE_REAL);
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
    Open(services, activity, state);
}

void EnterResolving(ActivityServices& services, Activity& activity, DuelGatheringState& state)
{
    activity.SetFlags(ACTIVITY_VIEW_CELEBRATE);
    activity.SetTravel(ActivityTravel::None);
    activity.SetPhase(ActivityPhase::Resolving, services.nowMs);
    Close(services, activity, state);
}
}  // namespace

DuelGatheringKind::SpotCheck const& DuelGatheringKind::Check(ActivityDuelSpot const& spot)
{
    std::string const key = Acore::StringFormat("{}:{}:{}:{}:{}", spot.name, spot.mapId, spot.x, spot.y, spot.z);
    auto const known = spotChecks.find(key);
    if (known != spotChecks.end())
        return known->second;

    SpotCheck check;
    std::string bad;
    MapEntry const* entry = sMapStore.LookupEntry(spot.mapId);
    // CreateBaseMap asserts on an unknown map; an instanced map has no single base map to stand on.
    Map* map = entry && !entry->Instanceable() ? sMapMgr->CreateBaseMap(spot.mapId) : nullptr;
    if (!map)
        bad = Acore::StringFormat("map {} is not an open world map", spot.mapId);
    else
    {
        float const ground = map->GetHeight(PHASEMASK_NORMAL, spot.x, spot.y, spot.z + 10.0f);
        AreaTableEntry const* area =
            ground > INVALID_HEIGHT
                ? sAreaTableStore.LookupEntry(map->GetAreaId(PHASEMASK_NORMAL, spot.x, spot.y, ground))
                : nullptr;
        if (ground <= INVALID_HEIGHT)
            bad = "no ground at its coordinates";
        else if (!area)
            bad = "no area at its coordinates";
        else if (!(area->flags & AREA_FLAG_ALLOW_DUELS))
            bad = Acore::StringFormat("area {} does not allow duels", area->ID);
        else
        {
            check.good = true;
            check.areaId = area->ID;
            check.point.mapId = spot.mapId;
            check.point.x = spot.x;
            check.point.y = spot.y;
            check.point.z = ground + 0.05f;
            check.point.zoneId = map->GetZoneId(PHASEMASK_NORMAL, spot.x, spot.y, ground);
            check.point.set = true;
        }
    }

    if (check.good)
        LOG_DEBUG(LOG, "duel spot {} checked: area {}, zone {}, ground {}", spot.name, check.areaId, check.point.zoneId,
                  check.point.z);
    else
        LOG_WARN(LOG, "CoA activity: duel spot {} is not usable ({})", spot.name, bad);
    return spotChecks.emplace(key, check).first->second;
}

bool DuelGatheringKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                             std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    ActivityConfig const& config = services.config;
    Player* const nearPlayer = request.nearPlayer;

    // The spot: named, else the nearest usable one on the player's map (same faction, level bracket rule for
    // ambient rings), else for a GM a random usable one of the team.
    ActivityDuelSpot const* spot = nullptr;
    if (!request.target.empty())
    {
        spot = config.FindSpot(request.target);
        if (!spot || !Check(*spot).good)
        {
            why = "unknown or unusable spot " + request.target;
            return false;
        }
        if (request.team && *request.team != spot->team)
        {
            why = Acore::StringFormat("spot {} is for the {}", spot->name, TeamName(spot->team));
            return false;
        }
    }
    else
    {
        if (!nearPlayer && !request.gm)
        {
            why = "no real player to place it near";
            return false;
        }

        std::optional<uint8> const team =
            request.team ? request.team
                         : (nearPlayer ? std::optional<uint8>(uint8(nearPlayer->GetTeamId())) : std::nullopt);
        auto const fits = [&request](ActivityDuelSpot const& s)
        { return !request.level || (request.level >= s.minLevel && request.level <= s.maxLevel); };

        std::vector<ActivityDuelSpot const*> pool;
        float bestDist = std::numeric_limits<float>::max();
        bool bestFits = false;
        for (ActivityDuelSpot const& candidate : config.duelSpots)
        {
            if ((team && candidate.team != *team) || !Check(candidate).good)
                continue;
            pool.push_back(&candidate);
            if (!nearPlayer || candidate.mapId != nearPlayer->GetMapId())
                continue;
            uint8 const nearLevel = uint8(nearPlayer->GetLevel());
            if (!request.gm)
            {
                uint8 const level = ClampLevel(nearLevel, candidate.minLevel, candidate.maxLevel);
                int32 const gap = int32(ActivityBracket(level)) - int32(ActivityBracket(nearLevel));
                if (std::abs(gap) > int32(config.ambientNearPlayerBrackets))
                    continue;
            }
            bool const candidateFits = fits(candidate);
            float const dist = nearPlayer->GetExactDist2d(candidate.x, candidate.y);
            if (!spot || (candidateFits && !bestFits) || (candidateFits == bestFits && dist < bestDist))
            {
                spot = &candidate;
                bestDist = dist;
                bestFits = candidateFits;
            }
        }

        if (!spot && request.gm && !pool.empty())
        {
            std::vector<ActivityDuelSpot const*> fitting;
            for (ActivityDuelSpot const* candidate : pool)
                if (fits(*candidate))
                    fitting.push_back(candidate);
            std::vector<ActivityDuelSpot const*> const& from = fitting.empty() ? pool : fitting;
            spot = from[urand(0, uint32(from.size()) - 1)];
        }

        if (!spot)
        {
            why = nearPlayer ? "no usable duel spot of that faction on this map for that level" : "no usable duel spot";
            return false;
        }
    }

    SpotCheck const& check = Check(*spot);

    // The ring level: forced, the player's, or the level of a random free bot that could come.
    uint8 level = 0;
    if (request.level)
        level = ClampLevel(request.level, spot->minLevel, spot->maxLevel);
    else if (nearPlayer)
        level = ClampLevel(nearPlayer->GetLevel(), spot->minLevel, spot->maxLevel);
    else
    {
        ActivityCandidateQuery any;
        any.team = spot->team;
        any.minLevel = spot->minLevel;
        any.maxLevel = spot->maxLevel;
        any.center = check.point;
        any.limit = ANY_LEVEL_POOL;
        std::vector<ActivityCandidate> const free = services.candidates.Find(any);
        if (free.empty())
        {
            why = Acore::StringFormat("no free {} bot in levels {}-{}", TeamName(spot->team), spot->minLevel,
                                      spot->maxLevel);
            return false;
        }
        level = free[urand(0, uint32(free.size()) - 1)].level;
    }
    uint8 const minLevel = uint8(std::max<int32>(spot->minLevel, int32(level) - LEVEL_WINDOW));
    uint8 const maxLevel = uint8(std::min<int32>(spot->maxLevel, int32(level) + LEVEL_WINDOW));

    uint32 wanted = std::clamp(request.size ? request.size : config.duelMaxBots, config.duelMinBots,
                               std::max(config.duelMinBots, config.duelMaxBots));
    wanted = std::min(wanted, request.capacity);
    uint32 const needed = config.duelMinBots;
    if (wanted < needed)
    {
        why = Acore::StringFormat("only {} participant places left, a ring needs {}", request.capacity, needed);
        return false;
    }

    ActivityCandidateQuery query;
    query.team = spot->team;
    query.minLevel = minLevel;
    query.maxLevel = maxLevel;
    query.center = check.point;
    query.sameMapOnly = false;
    query.limit = 2 * wanted;
    std::vector<ActivityCandidate> found = services.candidates.Find(query);
    if (found.size() < needed)
    {
        why =
            Acore::StringFormat("only {} free bots of that faction in levels {}-{}", found.size(), minLevel, maxLevel);
        return false;
    }

    auto state = std::make_unique<DuelGatheringState>();
    state->spotName = spot->name;
    state->spotAreaId = check.areaId;

    plan.kind = ActivityKindId::Duel;
    plan.gm = request.gm;
    plan.team = spot->team;
    plan.minLevel = minLevel;
    plan.maxLevel = maxLevel;
    plan.rally = check.point;
    plan.target = spot->name;
    plan.name = spot->name;
    plan.wanted = wanted;
    plan.needed = needed;
    plan.leaderFixed = false;
    plan.candidates = std::move(found);
    plan.decisions.clear();
    plan.state = std::move(state);

    LOG_DEBUG(LOG, "duel plan: spot {} ({}), levels {}-{}, {} candidates, wanted {}, needed {}", spot->name,
              TeamName(spot->team), minLevel, maxLevel, plan.candidates.size(), wanted, needed);
    return true;
}

bool DuelGatheringKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (!dynamic_cast<DuelGatheringState*>(plan.state.get()) || !plan.rally.IsSet())
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

    // The gathering may use its whole timeout before the ring even opens.
    uint64 const start = activity.startedMs ? activity.startedMs : services.nowMs;
    uint64 const minutes = services.config.duelMinutes + 10 + ActivityThreshold::HARD_DEADLINE_EXTRA_MIN;
    activity.hardDeadlineMs = start + uint64(services.config.travelGatherTimeoutSeconds) * IN_MILLISECONDS +
                              minutes * MINUTE * IN_MILLISECONDS;

    activity.state = std::move(plan.state);
    activity.SetCenter(plan.rally, ActivityThreshold::RING_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
    return true;
}

void DuelGatheringKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;

    DuelGatheringState* state = StateOf(activity);
    if (!state)
    {
        activity.Finish(ActivityOutcome::TooFew, "duel ring without its state");
        return;
    }

    uint64 const now = services.nowMs;
    if (activity.phase != ActivityPhase::Resolving && activity.BotsIn() < 2)
    {
        activity.Finish(ActivityOutcome::TooFew,
                        Acore::StringFormat("spot {}: fewer than 2 bots left", state->spotName));
        return;
    }

    switch (activity.phase)
    {
        case ActivityPhase::Gathering:
        {
            uint32 arrived = 0;
            for (ActivityMember const& member : activity.members)
                if (member.In() && member.isBot && member.arrived)
                    ++arrived;

            if (arrived >= std::min(services.config.duelMinBots, activity.BotsIn()))
            {
                EnterActive(services, activity, *state);
                break;
            }
            if (now < activity.phaseSinceMs + uint64(services.config.travelGatherTimeoutSeconds) * IN_MILLISECONDS)
                break;

            if (arrived < 2)
            {
                activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("spot {}: only {} bots reached the ring",
                                                                             state->spotName, arrived));
                break;
            }
            for (ActivityMember const& member : activity.members)
                if (member.In() && member.isBot && !member.arrived)
                    activity.MarkLeft(member.guid, ActivityLeaveReason::Dropped, services.nowUnix);
            EnterActive(services, activity, *state);
            break;
        }
        case ActivityPhase::Active:
            if (!state->opened || !state->posted)
                Open(services, activity, *state);
            if (now >= state->activeUntilMs)
                EnterResolving(services, activity, *state);
            break;
        case ActivityPhase::Resolving:
            if (!state->closed)
                Close(services, activity, *state);
            if (now >= activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS)
                activity.Finish(ActivityOutcome::Success,
                                Acore::StringFormat("spot {}: {} duels among bots, {} with players", state->spotName,
                                                    state->duelsFought, state->duelsWithPlayers));
            break;
        default:
            break;
    }
}

void DuelGatheringKind::OnDuelResult(ActivityServices& /*services*/, Activity& activity,
                                     ActivityDuelResult const& result)
{
    COA_ASSERT_WORLD_THREAD();
    DuelGatheringState* state = StateOf(activity);
    ActivityMember* winner = activity.FindMember(result.winner);
    ActivityMember* loser = activity.FindMember(result.loser);
    bool const winnerIn = winner && winner->In();
    bool const loserIn = loser && loser->In();
    if (!state || (!winnerIn && !loserIn))
        return;

    if (winnerIn)
        ++winner->duelsWon;
    if (loserIn)
        ++loser->duelsLost;
    if (winnerIn && loserIn)
        ++state->duelsFought;
    else if ((winnerIn && !result.loserIsBot) || (loserIn && !result.winnerIsBot))
        ++state->duelsWithPlayers;

    // The ring's spectators applaud ("coa duel ended" reads it from the row).
    activity.ringDuelEndMs = result.endMs;
    activity.viewDirty = true;
    LOG_DEBUG(LOG, "activity #{} duel: {} beat {}{} (among bots {}, with players {})", activity.id,
              result.winner.GetCounter(), result.loser.GetCounter(), result.fled ? " (fled)" : "", state->duelsFought,
              state->duelsWithPlayers);
}

void DuelGatheringKind::OnMemberLeft(ActivityServices& /*services*/, Activity& activity,
                                     ActivityMember const& /*member*/)
{
    // Resolving ends by itself within RESOLVE_MS: it stays a success.
    if (activity.phase != ActivityPhase::Resolving && activity.BotsIn() < 2)
        activity.Finish(ActivityOutcome::TooFew, "fewer than 2 bots left");
}

std::string DuelGatheringKind::Join(ActivityServices& /*services*/, Activity& activity, Player* /*player*/)
{
    COA_ASSERT_WORLD_THREAD();
    DuelGatheringState const* state = StateOf(activity);
    std::string const area = ActivityWorld::AreaName(state ? state->spotAreaId : 0);
    return ActivityWorld::Line("coa_act_join_ring", "The duel ring is at %area (%zone): come and duel.",
                               {{"%area", area}, {"%zone", ActivityWorld::AreaName(activity.center.zoneId)}});
}

std::string DuelGatheringKind::Describe(Activity const& activity, uint64 nowMs) const
{
    DuelGatheringState const* state = dynamic_cast<DuelGatheringState const*>(activity.state.get());
    if (!state)
        return "duel ring without its state";

    uint64 endMs = activity.hardDeadlineMs;
    if (activity.phase == ActivityPhase::Active)
        endMs = state->activeUntilMs;
    else if (activity.phase == ActivityPhase::Resolving)
        endMs = activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS;
    uint64 const endsIn = endMs > nowMs ? (endMs - nowMs) / IN_MILLISECONDS : 0;

    std::string const area = ActivityWorld::AreaName(state->spotAreaId);
    return Acore::StringFormat("spot {} ({}), duels among bots {}, with players {}, ends in {} s", state->spotName,
                               area.empty() ? "?" : area, state->duelsFought, state->duelsWithPlayers, endsIn);
}

std::unique_ptr<ActivityKind> CreateDuelGatheringKind() { return std::make_unique<DuelGatheringKind>(); }
