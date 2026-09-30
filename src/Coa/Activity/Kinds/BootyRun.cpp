/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BootyRun.h"
#include "ActivityBrain.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "ActivityOrganizer.h"
#include "CoaThread.h"
#include "GameEventMgr.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include <algorithm>
#include <optional>
#include <vector>

namespace
{
constexpr char const* LOG = "playerbots.activity";
constexpr uint32 ARENA_MAP = 0;
constexpr float ARENA_X = -13202.9f;               // the chest's spawn point (Short John Mithril's script)
constexpr float ARENA_Y = 276.757f;
constexpr float ARENA_Z = 21.857f;
constexpr float ARENA_GATHER_RADIUS = 30.0f;       // Gathering: arrived in the arena
constexpr float ARENA_LEASH = 40.0f;               // Active: the participants stay this close to the center
constexpr uint64 CHEST_CHECK_MS = 5 * 1000;
constexpr float CHEST_WATCH_YARDS = 60.0f;         // a participant this close to the center looks for the chest
constexpr float CHEST_SEE_YARDS = 80.0f;
constexpr uint32 LINE_RETRY_MS = 15000;            // lines and the call are retried this long (budget, a fight)
constexpr uint32 MAX_SPEAKER_TRIES = 3;
// The way out: Short John Mithril's post (spawn 636), from where he walks down to the chest (waypoints 14508), so the
// path is walkable; 58 yards from the chest, on the ring above the pit.
constexpr float EXIT_X = -13184.5f;
constexpr float EXIT_Y = 332.671f;
constexpr float EXIT_Z = 40.33f;
constexpr float CHEST_GUARD_YARDS = 40.0f;  // out of the chest's reach (LootDistance 15 and a margin): the strategies'
                                            // guard for former participants (CoaActivityStrategy.cpp) uses the same
constexpr uint64 LEAVE_MAX_MS = 90 * 1000;  // the walk out ends then whoever is still near the chest

std::vector<uint32> const CLOSING_EMOTES = {TEXT_EMOTE_CHEER, TEXT_EMOTE_ROAR,    TEXT_EMOTE_FLEX,
                                            TEXT_EMOTE_DANCE, TEXT_EMOTE_VICTORY, TEXT_EMOTE_APPLAUD};

BootyRunState* StateOf(Activity& activity) { return dynamic_cast<BootyRunState*>(activity.state.get()); }

// A spoken line may start with a placeholder: the first ASCII letter goes upper case.
std::string Spoken(std::string text)
{
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
        text[0] = char(text[0] - 'a' + 'A');
    return text;
}

bool EventOn() { return sGameEventMgr->IsActiveEvent(uint16(ActivityThreshold::GURUBASHI_EVENT)); }

// Participants in the world, those within `range` of the arena's center first, each group shuffled; `zoneId` != 0
// keeps those in that zone (its channels).
std::vector<Player*> Participants(Activity const& activity, ActivityPoint const& arena, float range, uint32 zoneId = 0)
{
    std::vector<Player*> close;
    std::vector<Player*> others;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || !member.isBot)
            continue;
        Player* bot = ActivityWorld::FindMember(member);
        if (!bot || (zoneId && bot->GetZoneId() != zoneId))
            continue;
        bool const inside = bot->GetMapId() == arena.mapId &&
                            arena.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) <= range * range;
        (inside ? close : others).push_back(bot);
    }
    std::shuffle(close.begin(), close.end(), RandomEngine::Instance());
    std::shuffle(others.begin(), others.end(), RandomEngine::Instance());
    close.insert(close.end(), others.begin(), others.end());
    return close;
}

// Deferred (budget) and Refused (a fight) are tried again on the next ticks until `untilMs`.
bool TryAgain(ActivityServices const& services, ActivityOpResult result, uint64 untilMs)
{
    return (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused) && services.nowMs < untilMs;
}

// The booty call in the recruitment channels, once, under Chat.MaxPosts / MinGapSeconds.
void Call(ActivityServices& services, Activity& activity, BootyRunState& state)
{
    ActivityConfig const& config = services.config;
    if (config.chatChannel == ActivityChatChannel::None || !activity.CanPost(config, services.nowMs))
    {
        state.called = true;
        return;
    }

    bool const zone = config.chatChannel == ActivityChatChannel::Zone;
    std::string const text = Spoken(ActivityWorld::Line("coa_act_booty_call",
                                                        "The Gurubashi chest is up for grabs, see you in the arena!",
                                                        {{"%area", ActivityWorld::AreaName(state.areaId)}}));
    // In the zone channels of the real player of the team nearest to the arena, which the speaker joins when it is not
    // on them.
    ObjectGuid const audience =
        zone ? ActivityWorld::NearestAudience(services.realPlayers, activity.team, state.arena) : ObjectGuid::Empty;
    ActivityOpResult result = ActivityOpResult::Gone;
    uint32 tries = 0;
    for (Player* bot : Participants(activity, state.arena, ARENA_LEASH, zone ? state.arena.zoneId : 0))
    {
        if (tries++ >= MAX_SPEAKER_TRIES)
            break;
        result = zone ? services.ops.PostToChannels(bot->GetGUID(), config.chatChannels, text, state.arena.zoneId,
                                                    audience)
                      : services.ops.PostToChannel(bot->GetGUID(), ActivityChatTarget::World, text);
        if (result == ActivityOpResult::Done || result == ActivityOpResult::Deferred)
            break;
    }
    if (TryAgain(services, result, activity.phaseSinceMs + LINE_RETRY_MS))
        return;
    if (result == ActivityOpResult::Done)
        activity.NotePost(services.nowMs);
    LOG_DEBUG(LOG, "activity #{} gurubashi: call {}", activity.id, ActivityOpResultName(result));
    state.called = true;
}

// Every CHEST_CHECK_MS a participant in the arena looks for the chest: up once, then gone means someone opened it.
void WatchChest(ActivityServices& services, Activity& activity, BootyRunState& state)
{
    if (services.nowMs < state.nextChestCheckMs)
        return;
    state.nextChestCheckMs = services.nowMs + CHEST_CHECK_MS;

    std::vector<Player*> const watchers = Participants(activity, state.arena, CHEST_WATCH_YARDS);
    Player* watcher = watchers.empty() ? nullptr : watchers.front();
    // Nobody in the arena right now: no look this time.
    if (!watcher || watcher->GetMapId() != state.arena.mapId ||
        state.arena.SqDist2d(watcher->GetPositionX(), watcher->GetPositionY()) > CHEST_WATCH_YARDS * CHEST_WATCH_YARDS)
        return;

    bool const up =
        watcher->FindNearestGameObject(ActivityThreshold::GURUBASHI_CHEST, CHEST_SEE_YARDS, true) != nullptr;
    if (up && !state.chestSeen)
    {
        state.chestSeen = true;
        state.chestSeenMs = services.nowMs;
        LOG_DEBUG(LOG, "activity #{} gurubashi: the chest is up", activity.id);
    }
    else if (!up && state.chestSeen)
    {
        state.chestGone = true;
        LOG_DEBUG(LOG, "activity #{} gurubashi: the chest is gone", activity.id);
    }
}

// Once the chest is up, a participant in the arena wishes everyone luck.
void Cheer(ActivityServices& services, Activity& activity, BootyRunState& state)
{
    if (state.cheered || !state.chestSeen)
        return;

    std::string const text = Spoken(ActivityWorld::Line("coa_act_booty_cheer", "May the best one win!"));
    ActivityOpResult result = ActivityOpResult::Gone;
    uint32 tries = 0;
    for (Player* bot : Participants(activity, state.arena, ARENA_LEASH))
    {
        if (tries++ >= MAX_SPEAKER_TRIES)
            break;
        result = services.ops.PostToChannel(bot->GetGUID(), ActivityChatTarget::Say, text);
        if (result == ActivityOpResult::Done || result == ActivityOpResult::Deferred)
            break;
    }
    if (TryAgain(services, result, state.chestSeenMs + LINE_RETRY_MS))
        return;
    state.cheered = true;
}

void EnterActive(ActivityServices& services, Activity& activity, BootyRunState& state)
{
    state.activeUntilMs = services.nowMs + uint64(services.config.gurubashiMinutes) * MINUTE * IN_MILLISECONDS;
    state.nextChestCheckMs = services.nowMs;
    activity.SetCenter(state.arena, ARENA_LEASH);
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

// Every bot participant in the world is out of the chest's reach (or on another map).
bool AwayFromChest(Activity const& activity, BootyRunState const& state)
{
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || !member.isBot)
            continue;
        Player* bot = ActivityWorld::FindMember(member);
        if (bot && bot->GetMapId() == state.arena.mapId &&
            state.arena.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) <= CHEST_GUARD_YARDS * CHEST_GUARD_YARDS)
            return false;
    }
    return true;
}

// The end of Active. Gurubashi.BotsMayWin = 0 and the chest maybe still up (it outlasts the run and the event): the
// bots first walk out of its reach, as the run releases them with "loot" and "gather" back. Else Resolving at once.
void EndRun(ActivityServices& services, Activity& activity, BootyRunState& state)
{
    bool const chestMayBeUp = !state.chestGone && (state.chestSeen || EventOn());
    if (services.config.gurubashiBotsMayWin || !chestMayBeUp || !state.exit.IsSet())
    {
        EnterResolving(services, activity);
        return;
    }
    state.leaving = true;
    state.leaveUntilMs = services.nowMs + LEAVE_MAX_MS;
    activity.SetFlags(0);
    activity.SetCenter(state.exit, ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
}

// Walking out (Gathering after Active): Resolving at the exit once every bot is out of the chest's reach, the chest
// is gone, or LEAVE_MAX_MS passed (a bot still near it then keeps no "loot": CoaActivityStrategies::AddNonCombat).
void Leave(ActivityServices& services, Activity& activity, BootyRunState& state)
{
    WatchChest(services, activity, state);
    bool const away = AwayFromChest(activity, state);
    if (!away && !state.chestGone && services.nowMs < state.leaveUntilMs)
        return;
    LOG_DEBUG(LOG, "activity #{} gurubashi: out of the arena ({})", activity.id,
              away ? "all away" : (state.chestGone ? "the chest is gone" : "time up"));
    EnterResolving(services, activity);
}
}  // namespace

bool BootyRunKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                        std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    ActivityConfig const& config = services.config;
    Player* const p = request.nearPlayer;

    if (!EventOn())
    {
        if (!request.gm)
        {
            why = "the booty run is not on";
            return false;
        }
        LOG_DEBUG(LOG, "booty run started outside game_event {}: no chest", ActivityThreshold::GURUBASHI_EVENT);
    }

    uint32 const minLevelCfg = std::clamp<uint32>(config.gurubashiMinLevel, 1, 80);
    if (!request.gm)
    {
        if (!p)
        {
            why = "no real player to place it near";
            return false;
        }
        if (p->GetMapId() != ARENA_MAP || p->GetLevel() + 5 < minLevelCfg)
        {
            why = "the player is not in the Eastern Kingdoms or too low for the arena";
            return false;
        }
    }

    ActivityPoint center;
    center.mapId = ARENA_MAP;
    center.x = ARENA_X;
    center.y = ARENA_Y;
    center.z = ARENA_Z;
    center.set = true;
    ActivityPoint const arena = ActivityWorld::GroundNear(center, 0.0f);
    if (!arena.IsSet())
    {
        why = "no ground at the arena";
        return false;
    }
    uint32 areaId = 0;
    if (Map* map = sMapMgr->CreateBaseMap(ARENA_MAP))
        areaId = map->GetAreaId(PHASEMASK_NORMAL, arena.x, arena.y, arena.z);

    uint32 const level =
        request.level ? request.level : std::max<uint32>(p ? uint32(p->GetLevel()) : 0, minLevelCfg);
    uint8 const minLevel = uint8(std::clamp<int32>(std::max<int32>(int32(minLevelCfg), int32(level) - 5), 1, 80));
    uint8 const maxLevel = uint8(std::clamp<int32>(int32(level) + 5, int32(minLevel), 80));

    uint32 wanted = std::clamp(request.size ? request.size : config.gurubashiMaxBots, config.gurubashiMinBots,
                               std::max(config.gurubashiMinBots, config.gurubashiMaxBots));
    wanted = std::min(wanted, request.capacity);
    uint32 const needed = config.gurubashiMinBots;
    if (wanted < needed)
    {
        why = Acore::StringFormat("only {} participant places left, a booty run needs {}", request.capacity, needed);
        return false;
    }

    // Both factions, interleaved: the arena is free-for-all.
    ActivityCandidateQuery query;
    query.minLevel = minLevel;
    query.maxLevel = maxLevel;
    query.center = arena;
    query.limit = wanted;
    query.team = TEAM_ALLIANCE;
    std::vector<ActivityCandidate> const alliance = services.candidates.Find(query);
    query.team = TEAM_HORDE;
    std::vector<ActivityCandidate> const horde = services.candidates.Find(query);
    std::vector<ActivityCandidate> found;
    for (std::size_t i = 0; i < std::max(alliance.size(), horde.size()); ++i)
    {
        if (i < alliance.size())
            found.push_back(alliance[i]);
        if (i < horde.size())
            found.push_back(horde[i]);
    }
    if (found.size() < needed)
    {
        why = Acore::StringFormat("only {} free bots in levels {}-{}", found.size(), minLevel, maxLevel);
        return false;
    }

    auto state = std::make_unique<BootyRunState>();
    state->arena = arena;
    state->areaId = areaId;
    state->exit.mapId = ARENA_MAP;
    state->exit.x = EXIT_X;
    state->exit.y = EXIT_Y;
    state->exit.z = EXIT_Z;
    state->exit.zoneId = arena.zoneId;
    state->exit.set = true;

    std::string const area = ActivityWorld::AreaName(areaId);
    plan.kind = ActivityKindId::Gurubashi;
    plan.gm = request.gm;
    plan.team = request.team ? *request.team : (p ? uint8(p->GetTeamId()) : uint8(TEAM_ALLIANCE));
    plan.minLevel = minLevel;
    plan.maxLevel = maxLevel;
    plan.rally = arena;
    plan.target = "arena";
    plan.name = area.empty() ? std::string("Gurubashi Arena") : area;
    plan.wanted = wanted;
    plan.needed = needed;
    plan.leaderFixed = false;
    plan.candidates = std::move(found);
    plan.decisions.clear();
    plan.state = std::move(state);

    LOG_DEBUG(LOG, "gurubashi plan: levels {}-{}, {} alliance and {} horde candidates, wanted {}, needed {}", minLevel,
              maxLevel, alliance.size(), horde.size(), wanted, needed);
    return true;
}

bool BootyRunKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    BootyRunState* state = dynamic_cast<BootyRunState*>(plan.state.get());
    if (!state || !state->arena.IsSet())
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

    uint64 const minutes = services.config.gurubashiMinutes + 10 + ActivityThreshold::HARD_DEADLINE_EXTRA_MIN;
    activity.hardDeadlineMs = services.nowMs + minutes * MINUTE * IN_MILLISECONDS;

    activity.SetStyle(ACTIVITY_STYLE_ARENA);
    activity.SetCenter(state->arena, ARENA_GATHER_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
    activity.state = std::move(plan.state);
    return true;
}

void BootyRunKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;

    BootyRunState* state = StateOf(activity);
    if (!state)
    {
        activity.Finish(ActivityOutcome::TooFew, "booty run without its state");
        return;
    }
    // Walking out and Resolving end by themselves: the run stays a success.
    if (activity.phase != ActivityPhase::Resolving && !state->leaving && activity.BotsIn() < 2)
    {
        activity.Finish(ActivityOutcome::TooFew, "booty run: fewer than 2 bots left");
        return;
    }

    uint64 const now = services.nowMs;
    switch (activity.phase)
    {
        case ActivityPhase::Gathering:
        {
            if (state->leaving)
            {
                Leave(services, activity, *state);
                break;
            }
            uint32 arrived = 0;
            for (ActivityMember const& member : activity.members)
                if (member.In() && member.isBot && member.arrived)
                    ++arrived;
            if (arrived >= std::min(services.config.gurubashiMinBots, activity.BotsIn()))
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
                if (!bot || bot->GetMapId() != state->arena.mapId ||
                    state->arena.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) > stragglerYards * stragglerYards)
                    activity.MarkLeft(member.guid, ActivityLeaveReason::Dropped, services.nowUnix);
            }
            if (activity.BotsIn() < 2)
            {
                activity.Finish(ActivityOutcome::TooFew, "booty run: too few came to the arena");
                break;
            }
            EnterActive(services, activity, *state);
            break;
        }
        case ActivityPhase::Active:
            if (!state->called)
                Call(services, activity, *state);
            WatchChest(services, activity, *state);
            Cheer(services, activity, *state);
            if (state->chestGone || now >= state->activeUntilMs || (!activity.gm && !EventOn()))
                EndRun(services, activity, *state);
            break;
        case ActivityPhase::Resolving:
            Celebrate(services, activity);
            if (now >= activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS)
                activity.Finish(ActivityOutcome::Success,
                                state->chestGone ? "booty run: the chest was taken"
                                                 : (state->chestSeen ? "booty run: time up, the chest still there"
                                                                     : "booty run: no chest seen"));
            break;
        default:
            break;
    }
}

void BootyRunKind::OnMemberLeft(ActivityServices& /*services*/, Activity& activity, ActivityMember const& /*member*/)
{
    // Walking out and Resolving end by themselves: the run stays a success.
    BootyRunState const* state = StateOf(activity);
    if (activity.phase != ActivityPhase::Resolving && !(state && state->leaving) && activity.BotsIn() < 2)
        activity.Finish(ActivityOutcome::TooFew, "fewer than 2 bots left");
}

std::string BootyRunKind::Join(ActivityServices& /*services*/, Activity& activity, Player* /*player*/)
{
    COA_ASSERT_WORLD_THREAD();
    BootyRunState const* state = StateOf(activity);
    return ActivityWorld::Line("coa_act_join_spot", "They are at %area (%zone): come by!",
                               {{"%area", ActivityWorld::AreaName(state ? state->areaId : 0)},
                                {"%zone", ActivityWorld::AreaName(state ? state->arena.zoneId : 0)}});
}

std::string BootyRunKind::Describe(Activity const& activity, uint64 nowMs) const
{
    BootyRunState const* state = dynamic_cast<BootyRunState const*>(activity.state.get());
    if (!state)
        return "booty run without its state";

    uint64 endMs = activity.hardDeadlineMs;
    if (activity.phase == ActivityPhase::Active)
        endMs = state->activeUntilMs;
    else if (activity.phase == ActivityPhase::Gathering && state->leaving)
        endMs = state->leaveUntilMs + ActivityThreshold::RESOLVE_MS;
    else if (activity.phase == ActivityPhase::Resolving)
        endMs = activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS;
    uint64 const endsIn = endMs > nowMs ? (endMs - nowMs) / IN_MILLISECONDS : 0;

    char const* chest = state->chestGone ? "taken" : (state->chestSeen ? "up" : "not seen");
    return Acore::StringFormat("booty run at {} {:.0f} {:.0f}, event {}, chest {}{}, ends in {} s",
                               ActivityWorld::AreaName(state->areaId), state->arena.x, state->arena.y,
                               EventOn() ? "on" : "off", chest, state->leaving ? ", leaving the arena" : "", endsIn);
}

std::unique_ptr<ActivityKind> CreateBootyRunKind() { return std::make_unique<BootyRunKind>(); }
