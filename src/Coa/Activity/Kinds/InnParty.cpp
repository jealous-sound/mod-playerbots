/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "InnParty.h"
#include "ActivityBrain.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "ActivityOrganizer.h"
#include "CoaThread.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
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
constexpr float PARTY_LEASH = 12.0f;          // Active: the participants stay this close to the inn
constexpr float PARTY_GREET_YARDS = 12.0f;    // a real player this close to the spot is greeted once
constexpr uint32 PARTY_GREETINGS_MAX = 4;     // greeting lines per party
constexpr uint32 LINE_RETRY_MS = 15000;       // the call is retried this long (budget, a fight)
constexpr uint32 MAX_SPEAKER_TRIES = 3;       // participants tried for one post
constexpr uint32 ANY_LEVEL_POOL = 64;         // free bots looked at for a level when nobody sets one
constexpr uint32 OVERWORLD_MAPS[] = {0, 1, 530, 571};
constexpr uint32 FACTION_TEMPLATE_ALLIANCE = 1;  // FactionTemplate.dbc "PLAYER, Human": an Alliance player
constexpr uint32 FACTION_TEMPLATE_HORDE = 2;     // "PLAYER, Orc": a Horde player

std::vector<uint32> const CLOSING_EMOTES = {TEXT_EMOTE_WAVE, TEXT_EMOTE_BOW, TEXT_EMOTE_CHEER};

InnPartyState* StateOf(Activity& activity) { return dynamic_cast<InnPartyState*>(activity.state.get()); }

// A spoken line may start with a placeholder: the first ASCII letter goes upper case.
std::string Spoken(std::string text)
{
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
        text[0] = char(text[0] - 'a' + 'A');
    return text;
}

bool TeamFits(uint8 spotTeam, uint8 team) { return spotTeam == TEAM_NEUTRAL || spotTeam == team; }

// The zone and area of a point on an open world map (zone 0 and area 0 when unknown).
void LookUpZone(ActivityPoint& point, uint32& areaId)
{
    MapEntry const* entry = sMapStore.LookupEntry(point.mapId);
    Map* map = entry && !entry->Instanceable() ? sMapMgr->CreateBaseMap(point.mapId) : nullptr;
    if (!map)
        return;
    point.zoneId = map->GetZoneId(PHASEMASK_NORMAL, point.x, point.y, point.z);
    areaId = map->GetAreaId(PHASEMASK_NORMAL, point.x, point.y, point.z);
}

// Participants in the world, shuffled; `zoneId` != 0 keeps those in that zone (its channels).
std::vector<ObjectGuid> Speakers(Activity const& activity, uint32 zoneId)
{
    std::vector<ObjectGuid> speakers;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || !member.isBot)
            continue;
        Player* bot = ActivityWorld::FindMember(member);
        if (bot && (!zoneId || bot->GetZoneId() == zoneId))
            speakers.push_back(member.guid);
    }
    std::shuffle(speakers.begin(), speakers.end(), RandomEngine::Instance());
    return speakers;
}

// The Chat.Channel post by the first participant that can make it (Deferred: the budget ran out first). In the zone
// channels of the real player of the team nearest to the inn, which the speaker joins when it is not on them.
ActivityOpResult Post(ActivityServices& services, Activity const& activity, std::string const& text)
{
    ActivityConfig const& config = services.config;
    bool const zone = config.chatChannel == ActivityChatChannel::Zone;
    ObjectGuid const audience =
        zone ? ActivityWorld::NearestAudience(services.realPlayers, activity.team, activity.center) : ObjectGuid::Empty;
    ActivityOpResult last = ActivityOpResult::Gone;
    uint32 tries = 0;
    for (ObjectGuid const& speaker : Speakers(activity, zone ? activity.center.zoneId : 0))
    {
        if (tries++ >= MAX_SPEAKER_TRIES)
            break;
        last = zone ? services.ops.PostToChannels(speaker, config.chatChannels, text, activity.center.zoneId, audience)
                    : services.ops.PostToChannel(speaker, ActivityChatTarget::World, text);
        if (last == ActivityOpResult::Done || last == ActivityOpResult::Deferred)
            break;
    }
    return last;
}

// The party call in the recruitment channels, once, when a real player of the team is in the inn's zone.
void Call(ActivityServices& services, Activity& activity, InnPartyState& state)
{
    ActivityConfig const& config = services.config;
    uint32 const zoneId = state.spot.zoneId;
    uint8 const team = activity.team;
    bool const playerInZone =
        zoneId && std::any_of(services.realPlayers.begin(), services.realPlayers.end(),
                              [zoneId, team](Player* player)
                              { return player && player->GetZoneId() == zoneId && player->GetTeamId() == team; });
    if (config.chatChannel == ActivityChatChannel::None || !playerInZone || !activity.CanPost(config, services.nowMs))
    {
        state.called = true;
        return;
    }

    std::string const text = Spoken(ActivityWorld::Line("coa_act_party_call",
                                                        "Drinks at the inn in %area, come and join us!",
                                                        {{"%area", ActivityWorld::AreaName(state.areaId)}}));
    ActivityOpResult const result = Post(services, activity, text);
    if ((result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused) &&
        services.nowMs < activity.phaseSinceMs + LINE_RETRY_MS)
        return;
    if (result == ActivityOpResult::Done)
        activity.NotePost(services.nowMs);
    LOG_DEBUG(LOG, "activity #{} party: call {}", activity.id, ActivityOpResultName(result));
    state.called = true;
}

// A real player who walks in (within PARTY_GREET_YARDS of the spot): the participant nearest to it says hello,
// once per player, PARTY_GREETINGS_MAX times per party.
void Greet(ActivityServices& services, Activity& activity, InnPartyState& state)
{
    if (state.greetings >= PARTY_GREETINGS_MAX)
        return;

    float const rangeSq = PARTY_GREET_YARDS * PARTY_GREET_YARDS;
    for (Player* player : services.realPlayers)
    {
        if (!player || !player->IsInWorld() || !player->IsAlive() || player->IsGameMaster() ||
            player->GetMapId() != state.spot.mapId ||
            state.spot.SqDist2d(player->GetPositionX(), player->GetPositionY()) > rangeSq ||
            state.greetedMs.count(player->GetGUID().GetCounter()))
            continue;

        Player* speaker = nullptr;
        float best = std::numeric_limits<float>::max();
        for (ActivityMember const& member : activity.members)
        {
            if (!member.In() || !member.isBot)
                continue;
            Player* bot = ActivityWorld::FindMember(member);
            if (!bot || !bot->IsAlive() || bot->GetMap() != player->GetMap())
                continue;
            float const dist = bot->GetExactDist2d(player);
            if (dist < best)
            {
                best = dist;
                speaker = bot;
            }
        }
        // Nobody to say it, or a player of a faction the party is hostile to: not greeted.
        if (!speaker || speaker->IsHostileTo(player))
            continue;

        std::string const text =
            Spoken(ActivityWorld::Line("coa_act_party_greet", "Hey %player, pull up a chair!",
                                       {{"%player", ActivityWorld::FirstName(player)}}));
        ActivityOpResult const result = services.ops.PostToChannel(speaker->GetGUID(), ActivityChatTarget::Say, text);
        if (result == ActivityOpResult::Deferred)
            return;
        // Said, or not possible now (a fight): that player is not greeted again at this party.
        state.greetedMs[player->GetGUID().GetCounter()] = services.nowMs;
        if (result == ActivityOpResult::Done)
            ++state.greetings;
        LOG_DEBUG(LOG, "activity #{} party: greeting {} ({})", activity.id, player->GetName(),
                  ActivityOpResultName(result));
        return;  // one greeting per tick
    }
}

void EnterActive(ActivityServices& services, Activity& activity, InnPartyState& state)
{
    uint32 const minutes = services.config.partyMinutes;
    uint32 const lo = uint32(std::max<int32>(5, int32(minutes) - 5));
    state.activeUntilMs = services.nowMs + uint64(urand(lo, minutes + 5)) * MINUTE * IN_MILLISECONDS;
    activity.SetCenter(state.spot, PARTY_LEASH);
    activity.SetTravel(ActivityTravel::StayNearCenter);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
    Call(services, activity, state);
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

InnPartyKind::SpotCheck const& InnPartyKind::Check(ActivitySpot const& spot)
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
        LOG_DEBUG(LOG, "party spot {} checked: area {}, zone {}", spot.name, check.areaId, check.point.zoneId);
    }
    else
        LOG_WARN(LOG, "CoA activity: party spot {} is not usable ({})", spot.name,
                 map ? "no ground at its coordinates" : "not an open world map");
    return spotChecks.emplace(key, check).first->second;
}

void InnPartyKind::LoadInns()
{
    if (innsLoaded)
        return;
    innsLoaded = true;

    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (std::find(std::begin(OVERWORLD_MAPS), std::end(OVERWORLD_MAPS), uint32(data.mapid)) ==
            std::end(OVERWORLD_MAPS))
            continue;
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(data.id);
        if (!creature || !(creature->npcflag & UNIT_NPC_FLAG_INNKEEPER))
            continue;

        Inn inn;
        inn.spawnId = spawnId;
        inn.factionTemplate = creature->faction;
        inn.point.mapId = data.mapid;
        inn.point.x = data.posX;
        inn.point.y = data.posY;
        inn.point.z = data.posZ;
        inn.point.set = true;
        inns.push_back(inn);
    }
    LOG_DEBUG(LOG, "{} innkeepers for inn parties", inns.size());
}

bool InnPartyKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
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

    std::string spotName;
    ActivityPoint spot;
    uint32 areaId = 0;
    if (!request.target.empty())
    {
        ActivitySpot const* named = config.FindPartySpot(request.target);
        if (!named || !Check(*named).good)
        {
            why = "unknown or unusable party spot " + request.target;
            return false;
        }
        if (team && !TeamFits(named->team, *team))
        {
            why = Acore::StringFormat("party spot {} is not for that faction", named->name);
            return false;
        }
        if (!team)
            team = named->team == TEAM_NEUTRAL ? uint8(urand(0, 1)) : named->team;
        spotName = named->name;
        spot = Check(*named).point;
        areaId = Check(*named).areaId;
    }
    else if (!config.partySpots.empty())
    {
        // The nearest usable spot of the player's faction (or any) on its map.
        float const maxSq = float(config.partyMaxYards) * float(config.partyMaxYards);
        float bestSq = std::numeric_limits<float>::max();
        for (ActivitySpot const& candidate : config.partySpots)
        {
            if (candidate.mapId != p->GetMapId() || !TeamFits(candidate.team, *team) || spotInUse.count(candidate.name))
                continue;
            float const distSq = (candidate.x - p->GetPositionX()) * (candidate.x - p->GetPositionX()) +
                                 (candidate.y - p->GetPositionY()) * (candidate.y - p->GetPositionY());
            if (distSq > maxSq || distSq >= bestSq || !Check(candidate).good)
                continue;
            bestSq = distSq;
            spotName = candidate.name;
            spot = Check(candidate).point;
            areaId = Check(candidate).areaId;
        }
    }
    else
    {
        // The nearest innkeeper on the player's map that is not hostile to the bots' team: judged by the team's
        // player faction, not the player's own (a GM in GM mode is friendly to everyone; a GM may ask for the other
        // team).
        LoadInns();
        FactionTemplateEntry const* teamFaction = sFactionTemplateStore.LookupEntry(
            *team == TEAM_ALLIANCE ? FACTION_TEMPLATE_ALLIANCE : FACTION_TEMPLATE_HORDE);
        float const maxSq = float(config.partyMaxYards) * float(config.partyMaxYards);
        float bestSq = std::numeric_limits<float>::max();
        Inn const* best = nullptr;
        for (Inn const& inn : inns)
        {
            if (inn.point.mapId != p->GetMapId())
                continue;
            float const distSq = inn.point.SqDist2d(p->GetPositionX(), p->GetPositionY());
            if (distSq > maxSq || distSq >= bestSq)
                continue;
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(inn.factionTemplate);
            if (!faction || !teamFaction || faction->IsHostileTo(*teamFaction))
                continue;
            if (spotInUse.count("inn_" + std::to_string(inn.spawnId)))
                continue;
            bestSq = distSq;
            best = &inn;
        }
        if (best)
        {
            spotName = "inn_" + std::to_string(best->spawnId);
            spot = best->point;
            LookUpZone(spot, areaId);
        }
    }

    if (!spot.IsSet())
    {
        why = Acore::StringFormat("no free friendly inn within {} yards", config.partyMaxYards);
        return false;
    }
    if (spotInUse.count(spotName))
    {
        why = "a party is already at " + spotName;
        return false;
    }

    // The level window: forced, the player's, or a free bot's near the spot.
    uint8 level = request.level ? request.level : (p ? uint8(p->GetLevel()) : 0);
    if (!level)
    {
        ActivityCandidateQuery any;
        any.team = *team;
        any.minLevel = 1;
        any.maxLevel = 80;
        any.center = spot;
        any.limit = ANY_LEVEL_POOL;
        std::vector<ActivityCandidate> const pool = services.candidates.Find(any);
        if (pool.empty())
        {
            why = "no free bot of that faction";
            return false;
        }
        level = pool[urand(0, uint32(pool.size()) - 1)].level;
    }
    int32 const spread = int32(config.partyLevelSpread);
    uint8 const minLevel = uint8(std::clamp<int32>(int32(level) - spread, 1, 80));
    uint8 const maxLevel = uint8(std::clamp<int32>(int32(level) + spread, 1, 80));

    uint32 wanted = std::clamp(request.size ? request.size : config.partyMaxBots, config.partyMinBots,
                               std::max(config.partyMinBots, config.partyMaxBots));
    wanted = std::min(wanted, request.capacity);
    uint32 const needed = config.partyMinBots;
    if (wanted < needed)
    {
        why = Acore::StringFormat("only {} participant places left, a party needs {}", request.capacity, needed);
        return false;
    }

    ActivityCandidateQuery query;
    query.team = *team;
    query.minLevel = minLevel;
    query.maxLevel = maxLevel;
    query.center = spot;
    query.limit = 2 * wanted;
    std::vector<ActivityCandidate> found = services.candidates.Find(query);
    if (found.size() < needed)
    {
        why = Acore::StringFormat("only {} free bots of that faction in levels {}-{}", found.size(), minLevel,
                                  maxLevel);
        return false;
    }

    auto state = std::make_unique<InnPartyState>();
    state->spotName = spotName;
    state->spot = spot;
    state->areaId = areaId;
    state->style = urand(0, 99) < config.partyDancePercent ? ACTIVITY_STYLE_DANCE : ACTIVITY_STYLE_INN;

    std::string const area = ActivityWorld::AreaName(areaId);
    plan.kind = ActivityKindId::Party;
    plan.gm = request.gm;
    plan.team = *team;
    plan.minLevel = minLevel;
    plan.maxLevel = maxLevel;
    plan.rally = spot;
    plan.target = spotName;
    plan.name = area.empty() ? spotName : area;
    plan.wanted = wanted;
    plan.needed = needed;
    plan.leaderFixed = false;
    plan.candidates = std::move(found);
    plan.decisions.clear();

    LOG_DEBUG(LOG, "party plan: {} ({}), {}, levels {}-{}, {} candidates, wanted {}, needed {}", spotName, area,
              state->style == ACTIVITY_STYLE_DANCE ? "dance" : "inn", minLevel, maxLevel, plan.candidates.size(),
              wanted, needed);
    plan.state = std::move(state);
    return true;
}

bool InnPartyKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    InnPartyState* state = dynamic_cast<InnPartyState*>(plan.state.get());
    if (!state || !state->spot.IsSet() || spotInUse.count(state->spotName))
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

    uint64 const minutes =
        services.config.partyMinutes + 5 + 10 + ActivityThreshold::HARD_DEADLINE_EXTRA_MIN;
    activity.hardDeadlineMs = services.nowMs + minutes * MINUTE * IN_MILLISECONDS;
    spotInUse[state->spotName] = activity.id;

    activity.SetStyle(state->style);
    activity.SetCenter(state->spot, ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
    activity.state = std::move(plan.state);
    return true;
}

void InnPartyKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;

    InnPartyState* state = StateOf(activity);
    if (!state)
    {
        activity.Finish(ActivityOutcome::TooFew, "inn party without its state");
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
            if (arrived >= std::min(services.config.partyMinBots, activity.BotsIn()))
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
                                Acore::StringFormat("{}: too few came to the inn", state->spotName));
                break;
            }
            EnterActive(services, activity, *state);
            break;
        }
        case ActivityPhase::Active:
            if (!state->called)
                Call(services, activity, *state);
            Greet(services, activity, *state);
            if (now >= state->activeUntilMs)
                EnterResolving(services, activity);
            break;
        case ActivityPhase::Resolving:
            Celebrate(services, activity);
            if (now >= activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS)
                activity.Finish(ActivityOutcome::Success,
                                Acore::StringFormat("{}: {} party, {} greetings", state->spotName,
                                                    state->style == ACTIVITY_STYLE_DANCE ? "dance" : "inn",
                                                    state->greetings));
            break;
        default:
            break;
    }
}

void InnPartyKind::OnMemberLeft(ActivityServices& /*services*/, Activity& activity, ActivityMember const& /*member*/)
{
    // Resolving ends by itself within RESOLVE_MS: it stays a success.
    if (activity.phase != ActivityPhase::Resolving && activity.BotsIn() < 2)
        activity.Finish(ActivityOutcome::TooFew, "fewer than 2 bots left");
}

std::string InnPartyKind::Join(ActivityServices& /*services*/, Activity& activity, Player* /*player*/)
{
    COA_ASSERT_WORLD_THREAD();
    InnPartyState const* state = StateOf(activity);
    return ActivityWorld::Line("coa_act_join_spot", "They are at %area (%zone): come by!",
                               {{"%area", ActivityWorld::AreaName(state ? state->areaId : 0)},
                                {"%zone", ActivityWorld::AreaName(state ? state->spot.zoneId : 0)}});
}

void InnPartyKind::OnEnd(ActivityServices& /*services*/, Activity& activity)
{
    InnPartyState const* state = StateOf(activity);
    if (!state)
        return;
    auto const it = spotInUse.find(state->spotName);
    if (it != spotInUse.end() && it->second == activity.id)
        spotInUse.erase(it);
}

std::string InnPartyKind::Describe(Activity const& activity, uint64 nowMs) const
{
    InnPartyState const* state = dynamic_cast<InnPartyState const*>(activity.state.get());
    if (!state)
        return "inn party without its state";

    uint64 endMs = activity.hardDeadlineMs;
    if (activity.phase == ActivityPhase::Active)
        endMs = state->activeUntilMs;
    else if (activity.phase == ActivityPhase::Resolving)
        endMs = activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS;
    uint64 const endsIn = endMs > nowMs ? (endMs - nowMs) / IN_MILLISECONDS : 0;

    std::string const area = ActivityWorld::AreaName(state->areaId);
    return Acore::StringFormat("party at {} ({}), {}, greetings {}, ends in {} s", state->spotName,
                               area.empty() ? "?" : area, state->style == ACTIVITY_STYLE_DANCE ? "dance" : "inn",
                               state->greetings, endsIn);
}

std::unique_ptr<ActivityKind> CreateInnPartyKind() { return std::make_unique<InnPartyKind>(); }
