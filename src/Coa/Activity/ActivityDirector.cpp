/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityDirector.h"
#include "ActivityConfig.h"
#include "ActivityDuelCooldowns.h"
#include "ActivityGuards.h"
#include "ActivityPvePolicy.h"
#include "ActivityRegistry.h"
#include "BootyRun.h"
#include "BrainService.h"
#include "BrainSink.h"
#include "CharacterCache.h"
#include "CityLoiter.h"
#include "CoaActivityValues.h"
#include "CoaText.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DuelGathering.h"
#include "DungeonRun.h"
#include "EliteHunt.h"
#include "Escort.h"
#include "GangParty.h"
#include "Group.h"
#include "GroupMgr.h"
#include "InnParty.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "OutpostRaid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "RareScout.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "UpdateTime.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>
#include <limits>

namespace
{
constexpr char const* LOGGER = "playerbots.activity";
constexpr uint64 TELEPORT_RETRY_MS = 5 * 1000;
constexpr uint64 PRUNE_INTERVAL_MS = 60 * 1000;
constexpr uint64 GROUP_ADD_GRACE_MS = 60 * 1000;  // a member FormGroup could not add yet has this long to be added
constexpr uint32 FIRST_SCHEDULE_MAX_S = 120;
constexpr uint32 ORGANIZER_SINK_CAPACITY = 64;
constexpr uint32 JOIN_LEVEL_MARGIN = 5;
constexpr uint32 RAID_MEMBERS_MAX = 40;
constexpr uint64 RESTORE_INTERVAL_MS = 1000;  // activities off: how often the pending restores look for their bots
constexpr uint64 JOIN_REQUEST_HOLD_MS = 10 * 1000;  // a chat join request waits for budget at most this long
constexpr uint32 JOIN_REQUEST_OPS = 2;              // operations a chat join answer may need (invite + whisper)
constexpr float FAR_AWAY = std::numeric_limits<float>::infinity();

// Where a member is this tick.
enum class Presence : uint8
{
    Gone,  // logged out, or another life of the bot
    Busy,  // connected but not in a map (a far teleport, a loading screen): looked at again next tick
    Here,
};

Presence Locate(ActivityMember const& member, Player*& out)
{
    out = nullptr;
    Player* const player = ObjectAccessor::FindConnectedPlayer(member.guid);
    if (!player)
        return Presence::Gone;
    if (member.isBot)
    {
        PlayerbotAI* const ai = GET_PLAYERBOT_AI(player);
        if (!ai || ai->GetCoaEpoch() != member.epoch)
            return Presence::Gone;
    }
    if (!player->IsInWorld() || player->IsDuringRemoveFromWorld())
        return Presence::Busy;
    out = player;
    return Presence::Here;
}

void ReleaseProtectedMember(ActivityKindId kind, ActivityMember const& member)
{
    if (!member.isBot || !ActivityPvePolicy::Protected(kind))
        return;
    Player* const bot = ObjectAccessor::FindConnectedPlayer(member.guid);
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (ai && ai->GetCoaEpoch() == member.epoch)
        ActivityPvePolicy::Release(ai);
}

char const* TeamName(uint8 team)
{
    return team == TEAM_ALLIANCE ? "alliance" : team == TEAM_HORDE ? "horde" : "neutral";
}

char const* KindKey(ActivityKindId kind)
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return "Duel";
        case ActivityKindId::Gang:
            return "Gang";
        case ActivityKindId::Dungeon:
            return "Dungeon";
        case ActivityKindId::Hunt:
            return "Hunt";
        case ActivityKindId::Raid:
            return "Raid";
        case ActivityKindId::Party:
            return "Party";
        case ActivityKindId::Loiter:
            return "Loiter";
        case ActivityKindId::Gurubashi:
            return "Gurubashi";
        case ActivityKindId::Escort:
            return "Escort";
        case ActivityKindId::RareScout:
            return "RareScout";
        default:
            return "None";
    }
}

char const* TravelName(ActivityTravel travel)
{
    switch (travel)
    {
        case ActivityTravel::Gather:
            return "gather";
        case ActivityTravel::FollowLeader:
            return "follow";
        case ActivityTravel::StayNearCenter:
            return "stay";
        default:
            return "none";
    }
}

std::string FlagNames(uint8 flags)
{
    static std::pair<uint8, char const*> const Names[] = {
        {ACTIVITY_VIEW_MAY_CHALLENGE, "may-challenge"}, {ACTIVITY_VIEW_MAY_CHALLENGE_REAL, "may-challenge-real"},
        {ACTIVITY_VIEW_HOLD, "hold"},                   {ACTIVITY_VIEW_CELEBRATE, "celebrate"},
        {ACTIVITY_VIEW_HUNT, "hunt"},                   {ACTIVITY_VIEW_RECRUITING, "recruiting"},
        {ACTIVITY_VIEW_RETREAT, "retreat"},             {ACTIVITY_VIEW_QUIET, "quiet"}};
    std::string out;
    for (auto const& [flag, name] : Names)
        if (flags & flag)
            out += std::string(out.empty() ? "" : ",") + name;
    return out.empty() ? "none" : out;
}

// 2D distance on the same map, FAR_AWAY otherwise.
float Distance(Player* player, ActivityPoint const& point)
{
    if (!player || !point.set || player->GetMapId() != point.mapId)
        return FAR_AWAY;
    return std::sqrt(point.SqDist2d(player->GetPositionX(), player->GetPositionY()));
}

// Seconds from now to a CoaTime deadline, 0 when past.
uint64 SecondsLeft(uint64 deadlineMs, uint64 nowMs) { return deadlineMs > nowMs ? (deadlineMs - nowMs) / 1000 : 0; }

// An area or zone in English words (the brain's state), whatever the realm's DBC locale.
std::string EnglishAreaName(uint32 id)
{
    AreaTableEntry const* const entry = id ? sAreaTableStore.LookupEntry(id) : nullptr;
    if (!entry)
        return "";
    char const* name = entry->area_name[LOCALE_enUS];
    if (!name || !*name)
        name = entry->area_name[sWorld->GetDefaultDbcLocale()];
    return name ? name : "";
}

std::string EnglishWhere(ActivityPoint const& point)
{
    if (!point.set)
        return "";
    std::string const zone = EnglishAreaName(point.zoneId);
    uint32 areaId = 0;
    if (Map* const map = sMapMgr->CreateBaseMap(point.mapId))
        areaId = map->GetAreaId(PHASEMASK_NORMAL, point.x, point.y, point.z);
    std::string const area = areaId && areaId != point.zoneId ? EnglishAreaName(areaId) : "";
    if (area.empty())
        return zone;
    return zone.empty() ? area : area + ", " + zone;
}

std::string CharacterName(ObjectGuid guid)
{
    std::string name;
    if (!sCharacterCache->GetCharacterNameByGuid(guid, name))
        name = "#" + std::to_string(guid.GetCounter());
    return name;
}

std::string JoinLine(char const* name, char const* english, std::map<std::string, std::string> const& placeholders = {})
{
    return ActivityWorld::Line(name, english, placeholders);
}
}  // namespace

ActivityDirector& ActivityDirector::Instance()
{
    static ActivityDirector instance;
    return instance;
}

ActivityDirector::ActivityDirector() : config(ActivityConfig::Current()) {}

ActivityKind* ActivityDirector::KindOf(ActivityKindId kind) const
{
    std::size_t const index = std::size_t(kind);
    return index < kinds.size() ? kinds[index].get() : nullptr;
}

ActivityServices ActivityDirector::Services(uint64 nowMs, uint32 nowUnix)
{
    return ActivityServices{*config,     ops,     candidates, ActivityDuelCooldowns::Instance(), realPlayers,
                            nowMs,       nowUnix, organizer,  census};
}

// --- Lifecycle ---

void ActivityDirector::OnStartup()
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped)
        return;
    config = ActivityConfig::Current();
    if (!config->enable)
    {
        // Activities off: only the War Mode an earlier run could not take back (a crash, a stop while bots were
        // offline) is put back, at each bot's next login. Silent; nothing else runs.
        LoadPendingRestores();
        return;
    }
    StartRunning(false);
}

void ActivityDirector::OnConfigReload()
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped)
        return;
    config = ActivityConfig::Current();
    if (!config->enable)
    {
        if (Running())
            StopRunning(ActivityOutcome::Disabled);
        return;
    }
    if (!Running())
    {
        StartRunning(true);
        return;
    }

    duelRules.store(config->duelEnable, std::memory_order_relaxed);
    duelAmbient.store(config->duelEnable && config->duelAmbient, std::memory_order_relaxed);
    census.SetRaidMinLevel(uint8(config->raidMinLevel));
    std::vector<uint32> disabledM7;
    for (auto& [id, activity] : activities)
        if (!config->KindEnabled(activity->kind))
        {
            activity->Finish(ActivityOutcome::Disabled, std::string(ActivityKindName(activity->kind)) + " disabled");
            if (activity->kind == ActivityKindId::Escort || activity->kind == ActivityKindId::RareScout)
                disabledM7.push_back(id);
        }
    if (!disabledM7.empty())
    {
        CollectRealPlayers();
        ops.BeginTick(config->maxOpsPerTick, &realPlayers);
        organizer.BeginTick(config.get(), organizerSink, CoaTime::NowMs());
        for (uint32 const id : disabledM7)
            EndActivity(*activities.at(id), ActivityOutcome::Disabled, uint32(time(nullptr)));
    }
    for (auto it = plans.begin(); it != plans.end();)
    {
        ActivityKindId const kind = it->plan.kind;
        if ((kind == ActivityKindId::Escort || kind == ActivityKindId::RareScout) && !config->KindEnabled(kind))
        {
            if (it->jobId && !it->answered)
                BrainService::Current()->Cancel(it->jobId);
            it = plans.erase(it);
            dirty = true;
        }
        else
            ++it;
    }
    if (!config->escortEnable)
    {
        escortRequestAtMs.clear();
        escortCooldownUntilMs.clear();
    }
    if (dirty)
    {
        RebuildHeld();
        Publish();
    }
}

void ActivityDirector::StartRunning(bool fromReload)
{
    COA_ASSERT_WORLD_THREAD();
    if (Running())
        return;

    ActivityStore& store = ActivityStore::Instance();
    if (!store.CheckTables())
    {
        if (!tablesMissingLogged)
        {
            LOG_ERROR(LOGGER, "CoA activity: table playerbots_coa_activity or playerbots_coa_activity_member is missing "
                              "(apply data/sql/playerbots/updates/2026_09_30_03_coa_activity.sql); activities stay off");
            tablesMissingLogged = true;
        }
        return;
    }

    uint64 const nowMs = CoaTime::NowMs();
    uint32 const nowUnix = uint32(time(nullptr));

    // Crash recovery: what a previous run left live is aborted; rulesets it changed are put back at login.
    store.AbortLiveRows(nowUnix, config->logKeepDays);
    for (ActivityRestoreRow const& row : store.LoadUnrestored())
        pendingRestore.emplace(row.guid, row.prevRuleset);
    SyncRestoring();
    // Never below an id this process already used (its inserts may still wait in the async queue).
    nextId = std::max(nextId, store.LoadMaxId() + 1);

    if (!kinds[std::size_t(ActivityKindId::Duel)])
        kinds[std::size_t(ActivityKindId::Duel)] = CreateDuelGatheringKind();
    if (!kinds[std::size_t(ActivityKindId::Gang)])
        kinds[std::size_t(ActivityKindId::Gang)] = CreateGangPartyKind();
    if (!kinds[std::size_t(ActivityKindId::Dungeon)])
        kinds[std::size_t(ActivityKindId::Dungeon)] = CreateDungeonRunKind();
    if (!kinds[std::size_t(ActivityKindId::Hunt)])
        kinds[std::size_t(ActivityKindId::Hunt)] = CreateEliteHuntKind();
    if (!kinds[std::size_t(ActivityKindId::Raid)])
        kinds[std::size_t(ActivityKindId::Raid)] = CreateOutpostRaidKind();
    if (!kinds[std::size_t(ActivityKindId::Party)])
        kinds[std::size_t(ActivityKindId::Party)] = CreateInnPartyKind();
    if (!kinds[std::size_t(ActivityKindId::Loiter)])
        kinds[std::size_t(ActivityKindId::Loiter)] = CreateCityLoiterKind();
    if (!kinds[std::size_t(ActivityKindId::Gurubashi)])
        kinds[std::size_t(ActivityKindId::Gurubashi)] = CreateBootyRunKind();
    if (!kinds[std::size_t(ActivityKindId::Escort)])
        kinds[std::size_t(ActivityKindId::Escort)] = CreateEscortKind();
    if (!kinds[std::size_t(ActivityKindId::RareScout)])
        kinds[std::size_t(ActivityKindId::RareScout)] = CreateRareScoutKind();
    census.SetRaidMinLevel(uint8(config->raidMinLevel));
    if (!organizerSink)
        organizerSink = std::make_shared<BrainSink>(ORGANIZER_SINK_CAPACITY);

    nextScheduleMs = nowMs + uint64(std::min(config->scheduleSeconds, FIRST_SCHEDULE_MAX_S)) * 1000;
    nextRetentionMs = nowMs + ActivityThreshold::RETENTION_INTERVAL_MS;
    nextPruneMs = nowMs + PRUNE_INTERVAL_MS;
    elapsedMs = 0;
    overLoadSinceMs = 0;
    lastLoadAbortMs = 0;
    noPlayerSinceMs = 0;
    suspended = false;
    paused = false;
    schedule = PendingSchedule();

    running.store(true, std::memory_order_relaxed);
    duelRules.store(config->duelEnable, std::memory_order_relaxed);
    duelAmbient.store(config->duelEnable && config->duelAmbient, std::memory_order_relaxed);
    auto const onOff = [](bool on) { return on ? "on" : "off"; };
    LOG_INFO(LOGGER,
             "CoA activities on: duel {}, gang {}, {} duel spots, dungeon {}, hunt {}, raid {}, party {}, loiter {}, "
             "gurubashi {}",
             onOff(config->duelEnable), onOff(config->gangEnable), config->duelSpots.size(),
             onOff(config->dungeonEnable), onOff(config->huntEnable), onOff(config->raidEnable),
             onOff(config->partyEnable), onOff(config->loiterEnable), onOff(config->gurubashiEnable));
    LOG_DEBUG(LOGGER, "CoA activity director started from {}", fromReload ? "a config reload" : "startup");
    if (config->escortEnable || config->rareScoutEnable)
        LOG_INFO(LOGGER, "CoA M7 activities: escort {}, rare scout {}", onOff(config->escortEnable),
                 onOff(config->rareScoutEnable));
}

void ActivityDirector::StopRunning(ActivityOutcome outcome)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Running())
        return;

    uint32 const nowUnix = uint32(time(nullptr));
    CollectRealPlayers();
    ops.BeginTick(config->maxOpsPerTick, &realPlayers);
    organizer.BeginTick(config.get(), organizerSink, CoaTime::NowMs());

    std::vector<uint32> ids;
    ids.reserve(activities.size());
    for (auto const& [id, activity] : activities)
        ids.push_back(id);
    for (uint32 const id : ids)
    {
        auto const it = activities.find(id);
        if (it == activities.end())
            continue;
        it->second->Finish(outcome);
        EndActivity(*it->second, outcome, nowUnix);
    }

    std::shared_ptr<IBrain> const brain = BrainService::Current();
    for (PendingPlan const& plan : plans)
        if (plan.jobId && !plan.answered)
            brain->Cancel(plan.jobId);
    plans.clear();
    if (schedule.active && schedule.jobId && !schedule.answered)
        brain->Cancel(schedule.jobId);
    schedule = PendingSchedule();
    organizer.Clear();
    ActivityRecruitment::Clear();
    heldRequests.clear();
    recruitOffers.clear();

    held.clear();
    dirty = true;
    Publish();
    // Bots back in the world since the last tick get their ruleset now; the others at their next login (Restoring()
    // keeps Update coming while activities are off).
    RestorePending();

    ActivityDuelCooldowns::Instance().Clear();
    ActivityStore::Instance().Reset();
    botCooldownUntilMs.clear();
    escortRequestAtMs.clear();
    escortCooldownUntilMs.clear();
    storedLeaders.clear();
    groupSinceMs.clear();
    inGroupSeen.clear();

    running.store(false, std::memory_order_relaxed);
    duelRules.store(false, std::memory_order_relaxed);
    duelAmbient.store(false, std::memory_order_relaxed);
    LOG_INFO(LOGGER, "CoA activities off");
}

void ActivityDirector::Shutdown()
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped)
        return;
    if (Running())
        StopRunning(ActivityOutcome::Shutdown);
    else if (Restoring())
    {
        // Activities off: the bots still in the world get their ruleset back now, the others at the next start
        // (their rows stay unrestored).
        ops.BeginTick(config->maxOpsPerTick, nullptr);
        RestorePending();
    }
    organizer.Clear();
    ActivityRecruitment::Clear();
    heldRequests.clear();
    escortRequestAtMs.clear();
    escortCooldownUntilMs.clear();
    // The sinks exist only once the director ran (never with activities off: nothing to close then).
    if (organizerSink)
    {
        ActivityDuelCooldowns::Instance().Shutdown();
        organizerSink->Close();
    }
    stopped = true;
    running.store(false, std::memory_order_relaxed);
    duelRules.store(false, std::memory_order_relaxed);
    duelAmbient.store(false, std::memory_order_relaxed);
    restoring.store(false, std::memory_order_relaxed);
}

// --- The tick ---

void ActivityDirector::Update(uint32 diff)
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped)
        return;

    config = ActivityConfig::Current();
    if (!config->enable)
    {
        // Safety net: the reload already stopped it.
        if (Running())
            StopRunning(ActivityOutcome::Disabled);
        // War Mode an activity gave comes back off even with activities off, at the bot's next login.
        if (Restoring())
        {
            elapsedMs += diff;
            if (elapsedMs >= RESTORE_INTERVAL_MS)
            {
                elapsedMs = 0;
                ops.BeginTick(config->maxOpsPerTick, nullptr);
                RestorePending();
            }
        }
        return;
    }
    if (!Running())
        return;

    // Duel accepts wait for the brain in seconds: drained on every world tick.
    ActivityDuelCooldowns::Instance().DrainBrain(config->brainDuelAcceptThreshold);

    elapsedMs += diff;
    if (elapsedMs < config->tickMs)
        return;
    elapsedMs = 0;
    Tick(CoaTime::NowMs(), uint32(time(nullptr)));
}

void ActivityDirector::Tick(uint64 nowMs, uint32 nowUnix)
{
    ActivityStore& store = ActivityStore::Instance();

    CollectRealPlayers();
    ops.BeginTick(config->maxOpsPerTick, &realPlayers);
    organizer.BeginTick(config.get(), organizerSink, nowMs);
    RebuildHeld();
    candidates.BeginTick(nowMs, &held, &botCooldownUntilMs);
    store.ProcessCallbacks();

    LoadGuard(nowMs);
    if (!escortRequestAtMs.empty() || !escortCooldownUntilMs.empty())
        PruneEscortRequests(nowMs);

    if (realPlayers.empty())
    {
        if (!noPlayerSinceMs)
            noPlayerSinceMs = nowMs;
    }
    else
        noPlayerSinceMs = 0;

    RouteDuelResults(nowMs, nowUnix);
    DrainOrganizer();
    RestorePending();
    JoinRequests(nowMs, nowUnix);

    std::vector<uint32> ids;
    ids.reserve(activities.size());
    for (auto const& [id, activity] : activities)
        ids.push_back(id);

    for (uint32 const id : ids)
    {
        auto const it = activities.find(id);
        if (it == activities.end())
            continue;
        Activity& activity = *it->second;

        MemberPass(activity, nowMs, nowUnix);
        HandleLeaves(activity, nowUnix);
        if (!activity.finishing)
        {
            if (ActivityKind* const kind = KindOf(activity.kind))
            {
                ActivityServices services = Services(nowMs, nowUnix);
                kind->Tick(services, activity);
            }
            HandleLeaves(activity, nowUnix);
        }

        if (activity.finishing)
        {
            EndActivity(activity, activity.outcome, nowUnix);
            continue;
        }
        SyncStore(activity);
        if (activity.viewDirty || activity.membershipDirty)
            dirty = true;
    }

    ResolvePlans(nowMs, nowUnix, false);
    ResolveSchedule(nowMs, nowUnix);
    Schedule(nowMs, nowUnix);

    census.Update(nowUnix, held);
    if (nowMs >= nextRetentionMs)
    {
        store.Retention(nowUnix, config->logKeepDays, nextId);
        nextRetentionMs = nowMs + ActivityThreshold::RETENTION_INTERVAL_MS;
    }
    if (nowMs >= nextPruneMs)
    {
        ActivityDuelCooldowns::Instance().Prune(nowMs, BrainClock::UtcDayNow());
        for (auto it = botCooldownUntilMs.begin(); it != botCooldownUntilMs.end();)
        {
            if (it->second <= nowMs)
                it = botCooldownUntilMs.erase(it);
            else
                ++it;
        }
        nextPruneMs = nowMs + PRUNE_INTERVAL_MS;
    }

    if (dirty)
        Publish();
}

void ActivityDirector::CollectRealPlayers()
{
    realPlayers.clear();
    for (Player* const player : sRandomPlayerbotMgr.GetPlayers())
        if (player && player->IsInWorld() && !player->IsDuringRemoveFromWorld() && player->GetSession() &&
            !player->GetSession()->IsBot())
            realPlayers.push_back(player);

    onlineBots = 0;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        if (it->second && it->second->IsInWorld())
            ++onlineBots;
}

void ActivityDirector::RebuildHeld()
{
    held.clear();
    for (auto const& [id, activity] : activities)
        for (ActivityMember const& member : activity->members)
            if (member.isBot && member.In())
                held.insert(member.guid.GetCounter());
    for (PendingPlan const& pending : plans)
        for (ActivityCandidate const& candidate : pending.plan.candidates)
            held.insert(candidate.guid.GetCounter());
}

void ActivityDirector::LoadGuard(uint64 nowMs)
{
    lastLoadMs = sWorldUpdateTime.GetMaxUpdateTimeOfCurrentTable();
    bool const wasSuspended = suspended;
    suspended = lastLoadMs > config->suspendAboveMs;
    if (suspended != wasSuspended)
        LOG_DEBUG(LOGGER, "activity load guard: {} ms, new activities {}", lastLoadMs,
                  suspended ? "suspended" : "allowed");

    if (lastLoadMs <= config->abortAboveMs)
    {
        overLoadSinceMs = 0;
        return;
    }
    if (!overLoadSinceMs)
        overLoadSinceMs = nowMs;
    if (nowMs < overLoadSinceMs + ActivityThreshold::LOAD_ABORT_HOLD_MS ||
        (lastLoadAbortMs && nowMs < lastLoadAbortMs + ActivityThreshold::LOAD_ABORT_HOLD_MS))
        return;

    // The largest live activity (bots in), the youngest on a tie.
    Activity* largest = nullptr;
    for (auto const& [id, activity] : activities)
    {
        if (activity->finishing)
            continue;
        if (!largest || activity->BotsIn() > largest->BotsIn() ||
            (activity->BotsIn() == largest->BotsIn() && activity->startedMs >= largest->startedMs))
            largest = activity.get();
    }
    if (!largest)
        return;
    largest->Finish(ActivityOutcome::LoadAbort, Acore::StringFormat("server load {} ms", lastLoadMs));
    lastLoadAbortMs = nowMs;
    LOG_WARN(LOGGER, "CoA activity: world update time {} ms above AbortAboveMs {} for {} s: ending activity #{}",
             lastLoadMs, config->abortAboveMs, ActivityThreshold::LOAD_ABORT_HOLD_MS / 1000, largest->id);
}

void ActivityDirector::RouteDuelResults(uint64 nowMs, uint32 nowUnix)
{
    std::vector<ActivityDuelResult> results;
    ActivityDuelCooldowns::Instance().DrainResults(results);
    for (ActivityDuelResult const& result : results)
    {
        uint32 routed[2] = {0, 0};
        ObjectGuid const sides[2] = {result.winner, result.loser};
        for (std::size_t i = 0; i < 2; ++i)
        {
            auto const member = memberActivity.find(sides[i].GetCounter());
            if (member == memberActivity.end() || (i == 1 && member->second == routed[0]))
                continue;
            routed[i] = member->second;
            auto const it = activities.find(member->second);
            if (it == activities.end() || it->second->finishing)
                continue;
            if (ActivityKind* const kind = KindOf(it->second->kind))
            {
                ActivityServices services = Services(nowMs, nowUnix);
                kind->OnDuelResult(services, *it->second, result);
                if (it->second->viewDirty)
                    dirty = true;
            }
        }
    }
}

void ActivityDirector::DrainOrganizer()
{
    if (!organizerSink || organizerSink->Empty())
        return;
    std::vector<BrainResult> results;
    organizerSink->Drain(results);
    for (BrainResult const& result : results)
    {
        if (schedule.active && result.tag == schedule.tag)
        {
            schedule.answered = true;
            schedule.pick = ActivityBrain::ReadNext(result, schedule.options);
            continue;
        }
        bool planned = false;
        for (PendingPlan& pending : plans)
        {
            if (pending.planId != result.tag || pending.answered)
                continue;
            std::vector<int8> const answers = ActivityBrain::ReadJoin(result, pending.asked.size());
            for (std::size_t i = 0; i < answers.size(); ++i)
                if (answers[i] >= 0 && pending.asked[i] < pending.plan.decisions.size())
                    pending.plan.decisions[pending.asked[i]] = answers[i];
            pending.answered = true;
            planned = true;
            LOG_DEBUG(LOGGER, "activity plan {}: join answers in ({})", pending.planId, BrainErrorName(result.error));
            break;
        }
        // The kinds' own questions (line key, retreat, closing emote); anything else is stale.
        if (!planned && result.tag >= ActivityOrganizer::ORGANIZER_TAG_BASE)
            organizer.Deliver(result);
    }
}

void ActivityDirector::RestorePending()
{
    if (pendingRestore.empty())
        return;
    ActivityStore& store = ActivityStore::Instance();
    for (auto it = pendingRestore.begin(); it != pendingRestore.end();)
    {
        // A bot back in an activity keeps what that activity gave it; its old ruleset comes back after.
        if (held.count(it->first))
        {
            ++it;
            continue;
        }
        Player* const bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(it->first));
        if (!bot || !bot->IsInWorld() || !bot->GetSession() || !bot->GetSession()->IsBot())
        {
            ++it;
            continue;
        }
        ActivityOpResult const result = ops.RestoreRuleset(bot->GetGUID(), it->second);
        if (result == ActivityOpResult::Done)
        {
            store.MarkRestored(it->first);
            it = pendingRestore.erase(it);
        }
        else
            ++it;
    }
    SyncRestoring();
}

void ActivityDirector::LoadPendingRestores()
{
    // No tables (activities never set up): nothing to restore, and nothing logged while activities are off.
    ActivityStore& store = ActivityStore::Instance();
    if (!store.CheckTables())
        return;
    for (ActivityRestoreRow const& row : store.LoadUnrestored())
        pendingRestore.emplace(row.guid, row.prevRuleset);
    SyncRestoring();
    if (!pendingRestore.empty())
        LOG_DEBUG(LOGGER, "CoA activities off: {} rulesets to restore at login", pendingRestore.size());
}

void ActivityDirector::SyncRestoring()
{
    restoring.store(!pendingRestore.empty(), std::memory_order_relaxed);
}

// --- Members ---

void ActivityDirector::MemberPass(Activity& activity, uint64 nowMs, uint32 nowUnix)
{
    bool const groupKind = ActivityKindIsGroup(activity.kind);
    bool const formed = groupKind && !activity.groupGuid.IsEmpty();
    if (formed)
        groupSinceMs.emplace(activity.id, nowMs);

    Player* leader = nullptr;
    if (ActivityMember* const leaderMember = activity.Leader(); leaderMember && leaderMember->In())
        Locate(*leaderMember, leader);

    // M5: a real player member the bots follow (the dungeon run once the player leads). A followed member who left
    // (or never was one) is dropped, and the leader rule below gives the bots their master back in this pass. In the
    // activity but not in the group yet: masters stay as they are.
    if (!activity.follow.IsEmpty())
    {
        ActivityMember const* const followedMember = activity.FindMember(activity.follow);
        if (!followedMember || !followedMember->In() || followedMember->isBot)
            activity.SetFollow(ObjectGuid::Empty);
    }
    Player* followed = nullptr;
    if (formed && !activity.follow.IsEmpty())
    {
        followed = activity.FollowedPlayer();
        Group* const group = sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
        if (followed && (!group || !group->IsMember(followed->GetGUID())))
            followed = nullptr;
    }

    for (std::size_t i = 0; i < activity.members.size(); ++i)
    {
        ActivityMember& member = activity.members[i];
        if (!member.In())
            continue;

        Player* player = nullptr;
        Presence const presence = Locate(member, player);
        if (presence == Presence::Gone)
        {
            activity.MarkLeft(member.guid, ActivityLeaveReason::Gone, nowUnix);
            continue;
        }
        if (presence == Presence::Busy)
            continue;

        // Real players: only their group membership is looked at. Never teleported, revived or given a ruleset.
        if (!member.isBot)
        {
            if (formed)
            {
                Group* group = player->GetOriginalGroup();
                if (!group)
                    group = player->GetGroup();
                if (!group || group->GetGUID() != activity.groupGuid)
                    activity.MarkLeft(member.guid, ActivityLeaveReason::Left, nowUnix);
            }
            continue;
        }

        uint32 const low = member.guid.GetCounter();
        Group* const group = player->GetGroup();
        bool inGroup = false;
        if (!formed)
        {
            if (group)
            {
                activity.MarkLeft(member.guid, ActivityLeaveReason::Grouped, nowUnix);
                continue;
            }
        }
        else if (group && group->GetGUID() == activity.groupGuid)
        {
            inGroup = true;
            inGroupSeen[activity.id].insert(low);
        }
        else if (group || inGroupSeen[activity.id].count(low) ||
                 nowMs >= groupSinceMs[activity.id] + GROUP_ADD_GRACE_MS)
        {
            activity.MarkLeft(member.guid, ActivityLeaveReason::Grouped, nowUnix);
            continue;
        }

        // The leader leads nobody's follower; followers follow the leader, whoever else joined or relogged. With a
        // followed player (M5) every bot, the leader too, follows that player.
        if (inGroup && !activity.follow.IsEmpty())
        {
            PlayerbotAI* const ai = GET_PLAYERBOT_AI(player);
            if (followed && ai->GetMaster() != followed)
            {
                ai->SetMaster(followed);
                ai->SetCoaStrategiesDirty();
            }
        }
        else if (inGroup)
        {
            PlayerbotAI* const ai = GET_PLAYERBOT_AI(player);
            if (member.role == ActivityRole::Leader)
            {
                if (ai->GetMaster())
                {
                    ai->SetMaster(nullptr);
                    ai->SetCoaStrategiesDirty();
                }
            }
            else if (leader && leader != player && ai->GetMaster() != leader)
            {
                ai->SetMaster(leader);
                ai->SetCoaStrategiesDirty();
            }
            else if (!leader)
            {
                // No leader to follow: a real player who left the group (a followed player, M5) is nobody's master.
                Player* const master = ai->GetMaster();
                if (master && master->GetSession() && !master->GetSession()->IsBot() &&
                    !group->IsMember(master->GetGUID()))
                {
                    ai->SetMaster(nullptr);
                    ai->SetCoaStrategiesDirty();
                }
            }
        }

        // Revive rule (the manager's own revival skips participants).
        if (!player->IsAlive())
        {
            if (activity.kind == ActivityKindId::Escort || activity.kind == ActivityKindId::RareScout)
            {
                activity.Finish(ActivityOutcome::TooFew, "a protected activity bot died");
                continue;  // M7 bots never receive the director's resurrection or recovery teleport
            }
            if (!member.deadSinceMs)
                member.deadSinceMs = nowMs;
            if (nowMs >= member.deadSinceMs + uint64(config->reviveAfterSeconds) * 1000)
            {
                ActivityPoint point;
                if (groupKind && leader && leader != player && leader->IsAlive())
                    point = ActivityWorld::PointOf(leader);
                else if (activity.center.set)
                    point = activity.center;
                if (point.set)
                {
                    ActivityOpResult const result = ops.Revive(member.guid, &point);
                    if (result == ActivityOpResult::Done)
                    {
                        member.deadSinceMs = 0;
                        member.lastTeleportMs = nowMs;
                    }
                }
            }
            continue;
        }
        member.deadSinceMs = 0;

        Travel(activity, member, player, leader, nowMs);
    }

    if (groupKind)
        GroupPass(activity, leader, nowMs);
    Invitations(activity, leader, nowMs, nowUnix);
    WarMode(activity, nowUnix);

    if (activity.hardDeadlineMs && nowMs >= activity.hardDeadlineMs)
        activity.Finish(ActivityOutcome::Timeout);
    else if (config->requireRealPlayerOnline && !activity.gm && noPlayerSinceMs &&
             nowMs >= noPlayerSinceMs + ActivityThreshold::NO_PLAYER_END_MS)
        activity.Finish(ActivityOutcome::NoPlayers);
}

void ActivityDirector::Travel(Activity& activity, ActivityMember& member, Player* bot, Player* leader, uint64 nowMs)
{
    if (activity.kind == ActivityKindId::Escort || activity.kind == ActivityKindId::RareScout)
    {
        member.stragglerSinceMs = 0;
        return;  // M7 movement belongs to its map-thread actions; no recovery teleport under any phase
    }
    bool const mayTeleport = !member.lastTeleportMs || nowMs >= member.lastTeleportMs + TELEPORT_RETRY_MS;
    auto const teleport = [&](ActivityPoint const& dest)
    {
        if (!dest.set)
            return ActivityOpResult::Failed;
        ActivityOpResult const result = ops.Teleport(member.guid, dest);
        if (result != ActivityOpResult::Deferred)
            member.lastTeleportMs = nowMs;
        return result;
    };
    // Straggler timing: far for StragglerSeconds, then a teleport (Done restarts the clock).
    auto const straggle = [&](bool tooFar, auto landing)
    {
        if (!tooFar)
        {
            member.stragglerSinceMs = 0;
            return;
        }
        if (!member.stragglerSinceMs)
            member.stragglerSinceMs = nowMs;
        if (mayTeleport && nowMs >= member.stragglerSinceMs + uint64(config->travelStragglerSeconds) * 1000 &&
            teleport(landing()) == ActivityOpResult::Done)
            member.stragglerSinceMs = 0;
    };

    switch (activity.travel)
    {
        case ActivityTravel::Gather:
        {
            if (!activity.center.set)
                break;
            float const distance = Distance(bot, activity.center);
            if (distance <= activity.radius)
            {
                member.arrived = true;
                break;
            }
            // Walkers are moved by "coa activity travel"; the far ones and those on another map are teleported.
            if (mayTeleport && distance > float(config->travelWalkMaxYards))
                teleport(ActivityWorld::GroundNear(activity.center, activity.radius / 2.0f));
            break;
        }
        case ActivityTravel::FollowLeader:
        {
            if (member.role == ActivityRole::Leader || !leader || leader == bot)
            {
                member.stragglerSinceMs = 0;
                break;
            }
            bool const tooFar = bot->GetMapId() != leader->GetMapId() ||
                                bot->GetExactDist2d(leader) > float(config->travelStragglerYards);
            straggle(tooFar, [leader]() { return ActivityWorld::GroundNear(ActivityWorld::PointOf(leader), 3.0f); });
            break;
        }
        case ActivityTravel::StayNearCenter:
        {
            if (!activity.center.set)
                break;
            bool const tooFar = Distance(bot, activity.center) > float(config->travelStragglerYards);
            straggle(tooFar, [&activity]()
                     { return ActivityWorld::GroundNear(activity.center, ActivityThreshold::RING_IDLE_RADIUS); });
            break;
        }
        default:
            member.stragglerSinceMs = 0;
            break;
    }
}

void ActivityDirector::GroupPass(Activity& activity, Player* leader, uint64 nowMs)
{
    if (activity.phase != ActivityPhase::Marching || activity.kind == ActivityKindId::Escort)
    {
        // Outside Marching the kind owns HOLD; the progress watch starts over at the next march.
        activity.leaderBestMs = 0;
        activity.holdSinceMs = 0;
        return;
    }

    bool const holding = activity.holdSinceMs != 0;
    ActivityMember* const leaderMember = activity.Leader();

    // Leader progress: no LEADER_PROGRESS_YARDS gained for 2 x StragglerSeconds (another map: never) -> a teleport
    // near the stop, under the visibility rule. A regroup hold is waiting by design, not a stall.
    if (leader && leaderMember && leader->IsAlive())
    {
        float const distance = Distance(leader, activity.center);
        if (holding || !activity.leaderBestMs)
        {
            activity.leaderBestDist = distance;
            activity.leaderBestMs = nowMs;
        }
        else if (distance + ActivityThreshold::LEADER_PROGRESS_YARDS < activity.leaderBestDist)
        {
            activity.leaderBestDist = distance;
            activity.leaderBestMs = nowMs;
        }
        else if (nowMs >= activity.leaderBestMs + 2 * uint64(config->travelStragglerSeconds) * 1000 &&
                 (!leaderMember->lastTeleportMs || nowMs >= leaderMember->lastTeleportMs + TELEPORT_RETRY_MS))
        {
            ActivityOpResult const result =
                ops.Teleport(leaderMember->guid, ActivityWorld::GroundNear(activity.center, activity.radius));
            if (result != ActivityOpResult::Deferred)
                leaderMember->lastTeleportMs = nowMs;
            if (result == ActivityOpResult::Done)
                activity.leaderBestMs = 0;
        }
    }

    // Regroup: the leader holds while fewer than REGROUP_PERCENT of the bots are near it.
    uint32 bots = 0;
    uint32 with = 0;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.isBot || !member.In())
            continue;
        ++bots;
        Player* const bot = leader ? ActivityWorld::FindMember(member) : nullptr;
        if (bot && bot->GetMapId() == leader->GetMapId() &&
            bot->GetExactDist2d(leader) <= ActivityThreshold::REGROUP_RADIUS)
            ++with;
    }
    bool const together = with * 100 >= bots * ActivityThreshold::REGROUP_PERCENT;
    if (!holding)
    {
        if (leader && bots && !together &&
            (!activity.holdEndedMs || nowMs >= activity.holdEndedMs + ActivityThreshold::REGROUP_GAP_MS))
        {
            activity.holdSinceMs = nowMs;
            activity.SetFlags(activity.viewFlags | ACTIVITY_VIEW_HOLD);
        }
    }
    else if (together || nowMs >= activity.holdSinceMs + ActivityThreshold::REGROUP_MAX_MS)
    {
        activity.SetFlags(activity.viewFlags & ~uint8(ACTIVITY_VIEW_HOLD));
        activity.holdEndedMs = nowMs;
        activity.holdSinceMs = 0;
    }
}

void ActivityDirector::Invitations(Activity& activity, Player* leader, uint64 nowMs, uint32 nowUnix)
{
    if (activity.invites.empty())
        return;
    Group* const group =
        activity.groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
    // Over a copy: the kind's OnPlayerJoined may change the invitations.
    std::vector<std::pair<ObjectGuid, uint64>> const invites(activity.invites.begin(), activity.invites.end());
    for (auto const& [guid, expiryMs] : invites)
    {
        Player* const player = ObjectAccessor::FindConnectedPlayer(guid);
        if (player && player->IsInWorld() && group && group->IsMember(guid))
        {
            activity.invites.erase(guid);
            ActivityMember const* const existing = activity.FindMember(guid);
            if (existing && existing->In())
                continue;
            activity.Enroll(guid, 0, false, player->GetLevel(), nowUnix);
            ActivityStore::Instance().InsertMember(activity.id, guid.GetCounter(), false, ActivityRole::Member,
                                                   nowUnix);
            LOG_DEBUG(LOGGER, "activity #{}: player {} joined", activity.id, player->GetName());

            // The kind may say its own welcome (M5); otherwise the leader's M4 whisper.
            bool welcomed = false;
            if (ActivityKind* const kind = KindOf(activity.kind))
            {
                ActivityMember const member = *activity.FindMember(guid);
                ActivityServices services = Services(nowMs, nowUnix);
                welcomed = kind->OnPlayerJoined(services, activity, member);
            }
            if (!welcomed && leader)
            {
                std::string const line =
                    JoinLine("coa_act_join_where", "Welcome! We are in %zone, come find us.",
                             {{"%zone", ActivityWorld::AreaName(activity.center.zoneId)}, {"%gang", activity.name}});
                ops.PostToChannel(leader->GetGUID(), ActivityChatTarget::Whisper, line, guid);
            }
        }
        else if (!player || nowMs >= expiryMs)
            activity.invites.erase(guid);
    }
}

void ActivityDirector::WarMode(Activity& activity, uint32 nowUnix)
{
    if (!activity.warMode || activity.phase < ActivityPhase::Gathering)
        return;
    for (std::size_t i = 0; i < activity.members.size(); ++i)
    {
        ActivityMember& member = activity.members[i];
        if (!member.isBot || !member.In() || member.rulesetApplied)
            continue;
        uint32 prev = 0;
        ActivityOpResult const result = ops.ApplyWarMode(member.guid, prev);
        if (result == ActivityOpResult::Done)
        {
            member.rulesetApplied = true;
            member.prevRuleset = prev;
            if (prev)
                ActivityStore::Instance().SetPrevRuleset(activity.id, member.guid.GetCounter(), prev);
        }
        else if (result == ActivityOpResult::Failed)
            // Never a High-Risk or Mercenary bot in a War Mode activity.
            activity.MarkLeft(member.guid, ActivityLeaveReason::Kicked, nowUnix);
    }
}

void ActivityDirector::RestoreRuleset(ActivityMember& member)
{
    if (!member.rulesetApplied || !member.prevRuleset)
        return;
    ActivityOpResult const result = ops.RestoreRuleset(member.guid, member.prevRuleset);
    if (result == ActivityOpResult::Done)
        ActivityStore::Instance().MarkRestored(member.guid.GetCounter());
    else if (result == ActivityOpResult::Gone)
    {
        pendingRestore[member.guid.GetCounter()] = member.prevRuleset;
        SyncRestoring();
    }
    member.rulesetApplied = false;
}

void ActivityDirector::HandleLeaves(Activity& activity, uint32 nowUnix)
{
    uint64 const nowMs = CoaTime::NowMs();
    ActivityStore& store = ActivityStore::Instance();
    ActivityKind* const kind = KindOf(activity.kind);

    // Index loop: the kind's OnMemberLeft may enroll or mark others.
    for (std::size_t i = 0; i < activity.members.size(); ++i)
    {
        if (activity.members[i].In() || activity.members[i].leftHandled)
            continue;
        ActivityMember& member = activity.members[i];
        member.leftHandled = true;
        activity.membershipDirty = true;
        store.MemberLeft(activity.id, member.guid.GetCounter(), member.leftUnix);

        if (member.isBot)
        {
            if (ActivityPvePolicy::Protected(activity.kind))
            {
                dirty = true;
                Publish();  // the dropped member is no longer protected before restoring its owned creatures
                ReleaseProtectedMember(activity.kind, member);
            }
            RestoreRuleset(member);
            // The zone channels it joined to recruit (PostToChannels): it keeps none of another place's.
            ops.LeaveActivityChannels(member.guid);
            // Offline members too: a bot that logged out must not come back into the activity's group.
            if (ActivityKindIsGroup(activity.kind) && !activity.groupGuid.IsEmpty())
                ops.Leave(activity.groupGuid, member.guid);
            botCooldownUntilMs[member.guid.GetCounter()] = nowMs + uint64(config->botCooldownMinutes) * 60000;
        }
        LOG_DEBUG(LOGGER, "activity #{}: {} {} left ({})", activity.id, member.isBot ? "bot" : "player",
                  member.guid.GetCounter(), ActivityLeaveReasonName(member.leftReason));

        if (kind)
        {
            ActivityMember const copy = member;
            ActivityServices services = Services(nowMs, nowUnix);
            kind->OnMemberLeft(services, activity, copy);
        }
    }
}

void ActivityDirector::SyncStore(Activity& activity)
{
    ActivityStore& store = ActivityStore::Instance();
    if (activity.phase != activity.storedPhase && activity.phase != ActivityPhase::Planning)
    {
        store.UpdatePhase(activity.id, activity.phase);
        activity.storedPhase = activity.phase;
    }
    uint32 const leaderLow = activity.leader.GetCounter();
    auto const stored = storedLeaders.find(activity.id);
    if (stored != storedLeaders.end() && stored->second != leaderLow)
    {
        store.UpdateLeader(activity.id, leaderLow);
        stored->second = leaderLow;
    }
}

void ActivityDirector::EndActivity(Activity& activityRef, ActivityOutcome outcome, uint32 nowUnix)
{
    auto const it = activities.find(activityRef.id);
    if (it == activities.end())
        return;

    // The activity leaves the map first: its rows are gone from the registry before anything else happens.
    std::unique_ptr<Activity> const owned = std::move(it->second);
    activities.erase(it);
    Activity& activity = *owned;
    dirty = true;
    Publish();

    uint64 const nowMs = CoaTime::NowMs();
    if (ActivityKind* const kind = KindOf(activity.kind))
    {
        ActivityServices services = Services(nowMs, nowUnix);
        kind->OnEnd(services, activity);
    }
    organizer.Forget(activity.id);
    recruitOffers.erase(recruitOffers.lower_bound({activity.id, ObjectGuid::Empty}),
                        recruitOffers.lower_bound({activity.id + 1, ObjectGuid::Empty}));

    ActivityStore& store = ActivityStore::Instance();
    uint32 bots = 0;
    uint32 players = 0;
    for (ActivityMember& member : activity.members)
    {
        if (member.isBot)
            ++bots;
        else
            ++players;
        if (member.In())
        {
            if (member.isBot)
                RestoreRuleset(member);
            store.MemberLeft(activity.id, member.guid.GetCounter(), nowUnix);
            member.leftUnix = nowUnix;
            member.leftReason = ActivityLeaveReason::Ended;
            member.leftHandled = true;
        }
        if (member.isBot)
        {
            ReleaseProtectedMember(activity.kind, member);
            // Every bot of the activity, also one that left since HandleLeaves last ran (a repeat finds nothing).
            ops.LeaveActivityChannels(member.guid);
            botCooldownUntilMs[member.guid.GetCounter()] = nowMs + uint64(config->botCooldownMinutes) * 60000;
        }
    }

    if (!activity.groupGuid.IsEmpty())
        ops.Disband(activity.groupGuid);

    std::string const summary = activity.summary.empty() ? ActivityOutcomeName(outcome) : activity.summary;
    store.EndActivity(activity.id, outcome, nowUnix, bots, players, summary);
    std::size_t const index = std::size_t(outcome);
    if (index < endedCount.size())
        ++endedCount[index];

    storedLeaders.erase(activity.id);
    groupSinceMs.erase(activity.id);
    inGroupSeen.erase(activity.id);
    LOG_INFO(LOGGER, "activity #{} {} ended: {}, {}", activity.id, ActivityKindName(activity.kind),
             ActivityOutcomeName(outcome), summary);
}

// --- Registry ---

void ActivityDirector::Publish()
{
    auto snapshot = std::make_shared<ActivityRegistrySnapshot>();
    std::unordered_map<uint32, uint64> published;
    memberActivity.clear();

    for (auto const& [id, activity] : activities)
    {
        if (activity->phase == ActivityPhase::Planning)
            continue;
        for (ActivityMember const& member : activity->members)
        {
            if (!member.isBot || !member.In())
                continue;
            ActivityMemberView view;
            view.activityId = id;
            view.epoch = member.epoch;
            view.kind = activity->kind;
            view.role = member.role;
            view.phase = activity->phase;
            view.phaseSinceMs = activity->phaseSinceMs;
            view.leaderLow = activity->leader.GetCounter();
            view.groupLow = activity->groupGuid.GetCounter();
            view.center = activity->center;
            view.radius = activity->radius;
            view.objective = activity->objective;
            view.ringDuelEndMs = activity->ringDuelEndMs;
            view.flags = activity->viewFlags;
            view.style = activity->viewStyle;
            view.emote = activity->closingEmote;

            uint32 const low = member.guid.GetCounter();
            snapshot->members[low] = view;
            published[low] = member.epoch;
            memberActivity[low] = id;
        }
        if (!activity->groupGuid.IsEmpty())
            snapshot->groups[activity->groupGuid.GetCounter()] = id;
        activity->viewDirty = false;
        activity->membershipDirty = false;
    }
    for (PendingPlan const& pending : plans)
        for (ActivityCandidate const& candidate : pending.plan.candidates)
            snapshot->reserved.insert(candidate.guid.GetCounter());

    ActivityRegistry::Instance().Publish(std::move(snapshot));
    dirty = false;

    // Strategy resets: every bot that appeared, left or changed life. Phase, center and flag changes need none (the
    // strategies read them from the row).
    for (auto const& [low, epoch] : published)
    {
        auto const before = lastPublished.find(low);
        if (before == lastPublished.end() || before->second != epoch)
            MarkStrategiesDirty(low, epoch, false);
    }
    for (auto const& [low, epoch] : lastPublished)
    {
        auto const now = published.find(low);
        if (now == published.end() || now->second != epoch)
            MarkStrategiesDirty(low, epoch, true);
    }
    lastPublished = std::move(published);
}

void ActivityDirector::MarkStrategiesDirty(uint32 botLow, uint64 epoch, bool anyEpoch) const
{
    Player* const bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botLow));
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (ai && (anyEpoch || ai->GetCoaEpoch() == epoch))
        ai->SetCoaStrategiesDirty();
}

// --- Capacity and scheduling ---

uint32 ActivityDirector::Participants() const
{
    uint32 count = 0;
    for (auto const& [id, activity] : activities)
        count += activity->BotsIn();
    return count;
}

uint32 ActivityDirector::ParticipantCap() const
{
    return std::min<uint32>(config->maxParticipants, uint32(uint64(onlineBots) * config->maxParticipantsPercent / 100));
}

uint32 ActivityDirector::Capacity() const
{
    uint32 used = Participants();
    for (PendingPlan const& pending : plans)
        used += uint32(pending.plan.candidates.size());
    uint32 const cap = ParticipantCap();
    return cap > used ? cap - used : 0;
}

uint32 ActivityDirector::LiveCount(ActivityKindId kind) const
{
    uint32 count = 0;
    for (auto const& [id, activity] : activities)
        if (activity->kind == kind)
            ++count;
    return count;
}

uint32 ActivityDirector::MinBots(ActivityKindId kind) const { return config->KindMinBots(kind); }

void ActivityDirector::PruneEscortRequests(uint64 nowMs)
{
    for (auto it = escortRequestAtMs.begin(); it != escortRequestAtMs.end();)
    {
        if (!ActivityRequestRules::RequestThrottled(it->second, nowMs))
            it = escortRequestAtMs.erase(it);
        else
            ++it;
    }
    for (auto it = escortCooldownUntilMs.begin(); it != escortCooldownUntilMs.end();)
    {
        if (nowMs >= it->second)
            it = escortCooldownUntilMs.erase(it);
        else
            ++it;
    }
}

bool ActivityDirector::RareTargetBusy(std::string const& target, uint8 team) const
{
    ActivityHuntTarget const* const chosen = config->FindRareScoutTarget(target);
    auto const same = [&](std::string const& otherName, uint8 otherTeam)
    {
        ActivityHuntTarget const* const other = config->FindRareScoutTarget(otherName);
        return ActivityRequestRules::SameRareTarget(target, team, otherName, otherTeam,
                                                    chosen ? chosen->spawnId : 0, other ? other->spawnId : 0);
    };
    for (auto const& [id, activity] : activities)
        if (activity->kind == ActivityKindId::RareScout && same(activity->target, activity->team))
            return true;
    for (PendingPlan const& pending : plans)
        if (pending.plan.kind == ActivityKindId::RareScout && same(pending.plan.target, pending.plan.team))
            return true;
    return false;
}

std::vector<ActivityKindId> ActivityDirector::EligibleKinds() const
{
    std::vector<ActivityKindId> eligible;
    if (activities.size() >= config->maxConcurrent)
        return eligible;
    uint32 const capacity = Capacity();
    for (uint8 k = uint8(ActivityKindId::None) + 1; k < uint8(ActivityKindId::Count); ++k)
    {
        ActivityKindId const kind = ActivityKindId(k);
        if (KindOf(kind) && config->KindEnabled(kind) && config->KindWeight(kind) > 0 &&
            LiveCount(kind) < config->KindMax(kind) && capacity >= MinBots(kind))
            eligible.push_back(kind);
    }
    return eligible;
}

Player* ActivityDirector::PickNear() const
{
    if (realPlayers.empty())
        return nullptr;
    // Players out in the world first: nothing is placed in a dungeon or a battleground.
    std::vector<Player*> outside;
    for (Player* const player : realPlayers)
    {
        uint32 const mapId = player->GetMapId();
        if (mapId == 0 || mapId == 1 || mapId == 530 || mapId == 571)
            outside.push_back(player);
    }
    std::vector<Player*> const& pool = outside.empty() ? realPlayers : outside;
    return pool[urand(0, uint32(pool.size()) - 1)];
}

ActivityBrain::Scene ActivityDirector::SceneNow() const
{
    ActivityBrain::Scene scene;
    scene.realPlayers = uint32(realPlayers.size());
    for (Player* const player : realPlayers)
    {
        if (scene.playerLevels.size() >= 8)
            break;
        scene.playerLevels.push_back(player->GetLevel());
    }
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        if (it->second && !held.count(it->second->GetGUID().GetCounter()) && ActivityCandidates::IsFree(it->second))
            ++scene.freeBots;
    scene.participants = Participants();
    for (auto const& [id, activity] : activities)
        if (std::size_t(activity->kind) < std::size(scene.running))
            ++scene.running[std::size_t(activity->kind)];
    scene.hourOfDay = Acore::Time::GetHours();
    return scene;
}

std::vector<ActivityBrain::NextOption> ActivityDirector::NextOptions(std::vector<ActivityKindId>& eligible,
                                                                     Player* nearPlayer, uint64 nowMs, uint32 nowUnix)
{
    std::vector<ActivityBrain::NextOption> options;
    std::vector<ActivityKindId> kept;
    kept.reserve(eligible.size());
    for (ActivityKindId const kind : eligible)
    {
        ActivityKind* const kindImpl = KindOf(kind);
        if (!kindImpl)
            continue;
        ActivityStartRequest request;
        request.kind = kind;
        request.nearPlayer = nearPlayer;
        request.capacity = Capacity();
        ActivityServices services = Services(nowMs, nowUnix);
        std::optional<std::vector<std::string>> const targets = kindImpl->Targets(services, request);
        if (!targets)
        {
            options.push_back({kind, ""});
            kept.push_back(kind);
            continue;
        }
        if (targets->empty())
        {
            LOG_DEBUG(LOGGER, "activity schedule: {} has no target now", ActivityKindName(kind));
            continue;
        }
        std::size_t const before = options.size();
        for (std::size_t i = 0; i < targets->size() && i < 4; ++i)
        {
            if (kind == ActivityKindId::RareScout)
            {
                ActivityHuntTarget const* const target = config->FindRareScoutTarget((*targets)[i]);
                if (!target || RareTargetBusy(target->name, target->team))
                    continue;
            }
            options.push_back({kind, (*targets)[i]});
        }
        if (options.size() > before)
            kept.push_back(kind);
    }
    eligible = std::move(kept);
    return options;
}

void ActivityDirector::Schedule(uint64 nowMs, uint32 nowUnix)
{
    if (paused || suspended || !plans.empty() || schedule.active || nowMs < nextScheduleMs)
        return;
    nextScheduleMs = nowMs + uint64(config->scheduleSeconds) * 1000 * urand(80, 120) / 100;
    if (config->requireRealPlayerOnline && realPlayers.empty())
        return;

    std::vector<ActivityKindId> eligible = EligibleKinds();
    if (eligible.empty())
        return;
    Player* const nearPlayer = PickNear();
    // Kinds with targets (hunts, raids) offer each target that fits now; a kind with none leaves the list.
    std::vector<ActivityBrain::NextOption> options = NextOptions(eligible, nearPlayer, nowMs, nowUnix);
    if (eligible.empty())
        return;

    if (config->useBrainOrganizer)
    {
        std::shared_ptr<IBrain> const brain = BrainService::Current();
        if (brain->Active(BrainUse::Organizer))
        {
            uint64 const tag = nextPlanId++;
            uint64 const deadline = nowMs + config->brainDeadlineMs;
            BrainTicket const ticket =
                brain->Submit(ActivityBrain::BuildNextJob(SceneNow(), options, tag, deadline), organizerSink, nullptr);
            if (ticket.status == BrainSubmit::Queued)
            {
                schedule = PendingSchedule();
                schedule.tag = tag;
                schedule.jobId = ticket.jobId;
                schedule.deadlineMs = deadline;
                schedule.eligible = eligible;
                schedule.options = std::move(options);
                schedule.nearGuid = nearPlayer ? nearPlayer->GetGUID() : ObjectGuid::Empty;
                schedule.active = true;
                LOG_DEBUG(LOGGER, "activity schedule: asked the brain ({} kinds eligible, {} options)", eligible.size(),
                          schedule.options.size());
                return;
            }
            LOG_DEBUG(LOGGER, "activity schedule: brain {}, dice", BrainSubmitName(ticket.status));
        }
    }
    DicePlan(eligible, nearPlayer);
}

void ActivityDirector::ResolveSchedule(uint64 nowMs, uint32 nowUnix)
{
    if (!schedule.active)
        return;
    if (!schedule.answered && nowMs < schedule.deadlineMs)
        return;

    PendingSchedule const done = std::move(schedule);
    schedule = PendingSchedule();
    if (!done.answered)
        BrainService::Current()->Cancel(done.jobId);
    if (paused || suspended || !plans.empty())
        return;

    Player* nearPlayer = done.nearGuid.IsEmpty() ? nullptr : ObjectAccessor::FindConnectedPlayer(done.nearGuid);
    if (nearPlayer && (!nearPlayer->IsInWorld() ||
                       std::find(realPlayers.begin(), realPlayers.end(), nearPlayer) == realPlayers.end()))
        nearPlayer = nullptr;
    if (!nearPlayer)
        nearPlayer = PickNear();
    if (config->requireRealPlayerOnline && !nearPlayer)
        return;

    std::vector<ActivityKindId> eligible = EligibleKinds();
    if (eligible.empty())
        return;

    if (done.answered && done.pick.valid)
    {
        if (done.pick.none)
        {
            LOG_DEBUG(LOGGER, "activity schedule: the brain chose nothing");
            return;
        }
        ActivityBrain::NextOption const& option = done.options[std::size_t(done.pick.index)];
        if (std::find(eligible.begin(), eligible.end(), option.kind) != eligible.end())
        {
            ActivityStartRequest request;
            request.kind = option.kind;
            request.nearPlayer = nearPlayer;
            request.capacity = Capacity();
            request.target = option.target;
            std::string why;
            if (!BeginPlan(request, config->useBrainOrganizer, why))
                LOG_DEBUG(LOGGER, "activity schedule: {} not planned: {}", ActivityKindName(request.kind), why);
            return;
        }
    }
    // The dice over the kinds that have something to do now (request.target stays empty: Plan chooses).
    NextOptions(eligible, nearPlayer, nowMs, nowUnix);
    if (eligible.empty())
        return;
    DicePlan(eligible, nearPlayer);
}

bool ActivityDirector::DicePlan(std::vector<ActivityKindId> const& eligible, Player* nearPlayer)
{
    uint32 total = 0;
    for (ActivityKindId const kind : eligible)
        total += config->KindWeight(kind);
    if (!total)
        return false;

    uint32 roll = urand(1, total);
    ActivityKindId chosen = eligible.back();
    for (ActivityKindId const kind : eligible)
    {
        uint32 const weight = config->KindWeight(kind);
        if (roll <= weight)
        {
            chosen = kind;
            break;
        }
        roll -= weight;
    }

    ActivityStartRequest request;
    request.kind = chosen;
    request.nearPlayer = nearPlayer;
    request.capacity = Capacity();
    std::string why;
    if (!BeginPlan(request, config->useBrainOrganizer, why))
    {
        LOG_DEBUG(LOGGER, "activity schedule: {} not planned: {}", ActivityKindName(chosen), why);
        return false;
    }
    return true;
}

bool ActivityDirector::BeginPlan(ActivityStartRequest const& request, bool askBrain, std::string& why)
{
    ActivityKind* const kind = KindOf(request.kind);
    if (!kind)
    {
        why = "unknown kind";
        return false;
    }

    uint64 const nowMs = CoaTime::NowMs();
    uint32 const nowUnix = uint32(time(nullptr));
    PendingPlan pending;
    pending.plan.kind = request.kind;
    pending.plan.gm = request.gm;
    {
        ActivityServices services = Services(nowMs, nowUnix);
        if (!kind->Plan(services, request, pending.plan, why))
        {
            LOG_DEBUG(LOGGER, "activity plan {}: {}", ActivityKindName(request.kind), why);
            return false;
        }
    }
    ActivityPlan& plan = pending.plan;
    plan.kind = request.kind;
    plan.gm = request.gm;
    if (plan.kind == ActivityKindId::RareScout && RareTargetBusy(plan.target, plan.team))
    {
        why = "that rare already has a scout or a pending scout plan";
        return false;
    }
    if (plan.candidates.empty())
    {
        why = "no free bot";
        return false;
    }

    plan.decisions.assign(plan.candidates.size(), int8(-1));
    if (plan.leaderFixed)
        plan.decisions[0] = 1;
    // Reserved at once: a GM start in the same tick cannot take them; the registry lists them at the next publish.
    for (ActivityCandidate const& candidate : plan.candidates)
        held.insert(candidate.guid.GetCounter());
    pending.planId = nextPlanId++;
    pending.answered = true;
    dirty = true;

    if (askBrain && config->useBrainOrganizer)
    {
        std::shared_ptr<IBrain> const brain = BrainService::Current();
        if (brain->Active(BrainUse::Organizer))
        {
            std::vector<ActivityBrain::Candidate> asked;
            for (uint32 i = 0; i < plan.candidates.size() && asked.size() < ActivityThreshold::MAX_JOIN_QUESTIONS;
                 ++i)
            {
                if (plan.decisions[i] >= 0)
                    continue;
                ActivityCandidate const& c = plan.candidates[i];
                ActivityBrain::Candidate question;
                question.level = c.level;
                question.classId = c.classId;
                question.bravery = c.bravery;
                question.sociability = c.sociability;
                question.pvpAppetite = c.pvpAppetite;
                question.distance = -1;
                if (plan.rally.set && c.mapId == plan.rally.mapId)
                    question.distance = int32(std::sqrt(plan.rally.SqDist2d(c.x, c.y)));
                asked.push_back(question);
                pending.asked.push_back(i);
            }
            if (!asked.empty())
            {
                uint64 const deadline = nowMs + config->brainDeadlineMs;
                BrainTicket const ticket = brain->Submit(
                    ActivityBrain::BuildJoinJob(plan.kind, SceneNow(), EnglishWhere(plan.rally), plan.minLevel,
                                                plan.maxLevel, asked, pending.planId, deadline),
                    organizerSink, nullptr);
                if (ticket.status == BrainSubmit::Queued)
                {
                    pending.jobId = ticket.jobId;
                    pending.deadlineMs = deadline;
                    pending.answered = false;
                }
                else
                    pending.asked.clear();
            }
        }
    }

    LOG_DEBUG(LOGGER, "activity plan {} {}: {} candidates, {} wanted, {} needed{}", pending.planId,
              ActivityKindName(plan.kind), plan.candidates.size(), plan.wanted, plan.needed,
              pending.answered ? "" : ", asking the brain");
    plans.push_back(std::move(pending));
    return true;
}

void ActivityDirector::ResolvePlans(uint64 nowMs, uint32 nowUnix, bool force)
{
    for (std::size_t i = 0; i < plans.size();)
    {
        PendingPlan& pending = plans[i];
        if (!force && !pending.answered && nowMs < pending.deadlineMs)
        {
            ++i;
            continue;
        }
        // Out of the list first: Capacity() then counts every other plan but not this one.
        PendingPlan resolved = std::move(pending);
        plans.erase(plans.begin() + std::ptrdiff_t(i));
        dirty = true;
        ResolvePlan(resolved, nowMs, nowUnix);
    }
}

uint32 ActivityDirector::ResolvePlan(PendingPlan& pending, uint64 nowMs, uint32 nowUnix)
{
    if (pending.jobId && !pending.answered)
        BrainService::Current()->Cancel(pending.jobId);

    ActivityPlan& plan = pending.plan;
    ActivityKind* const kind = KindOf(plan.kind);
    if (!kind || !config->KindEnabled(plan.kind))
        return 0;
    if (!plan.gm && (LiveCount(plan.kind) >= config->KindMax(plan.kind) || activities.size() >= config->maxConcurrent))
        return 0;
    if (plan.kind == ActivityKindId::RareScout && RareTargetBusy(plan.target, plan.team))
        return 0;

    // The dice for everyone the brain did not answer for.
    for (std::size_t i = 0; i < plan.candidates.size(); ++i)
    {
        if (plan.decisions[i] >= 0)
            continue;
        int32 const chance =
            std::clamp(int32(config->acceptPercent) + (int32(plan.candidates[i].sociability) - 50) / 2, 5, 95);
        plan.decisions[i] = int8(int32(urand(0, 99)) < chance ? 1 : 0);
    }

    // Still free now? (Bots that took an invite, fought, logged out or joined something else since.)
    std::unordered_set<uint32> members;
    for (auto const& [id, activity] : activities)
        for (ActivityMember const& member : activity->members)
            if (member.isBot && member.In())
                members.insert(member.guid.GetCounter());
    for (std::size_t i = 0; i < plan.candidates.size(); ++i)
    {
        if (plan.decisions[i] != 1)
            continue;
        ActivityCandidate const& candidate = plan.candidates[i];
        Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        bool const free = ai && ai->GetCoaEpoch() == candidate.epoch && ActivityCandidates::IsFree(bot) &&
                          !bot->IsInCombat() && !members.count(candidate.guid.GetCounter());
        if (free)
            continue;
        plan.decisions[i] = 0;
        if (plan.leaderFixed && i == 0)
        {
            LOG_DEBUG(LOGGER, "activity plan {}: the leader is no longer free", pending.planId);
            return 0;
        }
    }

    // Capacity, counted without this plan: the last accepted ones go first.
    uint32 accepted = 0;
    for (int8 const decision : plan.decisions)
        accepted += decision == 1 ? 1 : 0;
    uint32 const capacity = Capacity();
    for (std::size_t i = plan.decisions.size(); i-- > 0 && accepted > capacity;)
    {
        if (plan.decisions[i] != 1 || (plan.leaderFixed && i == 0))
            continue;
        plan.decisions[i] = 0;
        --accepted;
    }

    auto activity = std::make_unique<Activity>();
    activity->id = nextId++;
    activity->kind = plan.kind;
    activity->team = plan.team;
    activity->gm = plan.gm;
    activity->requester = plan.requester;
    activity->startedMs = nowMs;
    activity->startedUnix = nowUnix;
    activity->phaseSinceMs = nowMs;
    activity->storedPhase = ActivityPhase::Planning;
    {
        ActivityServices services = Services(nowMs, nowUnix);
        if (!kind->Begin(services, plan, *activity))
        {
            LOG_DEBUG(LOGGER, "activity plan {} {}: not enough bots accepted", pending.planId,
                      ActivityKindName(plan.kind));
            return 0;
        }
    }

    if (ActivityPvePolicy::Protected(activity->kind))
        for (ActivityMember const& member : activity->members)
            if (member.isBot && member.In())
            {
                Player* const bot = ActivityWorld::FindMember(member);
                if (PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr)
                    ActivityPvePolicy::Enroll(ai, activity->kind);
            }

    uint32 const id = activity->id;
    ActivityStore& store = ActivityStore::Instance();
    store.InsertActivity(id, activity->kind, activity->team, activity->target, activity->leader.GetCounter(),
                         activity->phase, activity->startedUnix);
    for (ActivityMember const& member : activity->members)
        store.InsertMember(id, member.guid.GetCounter(), member.isBot, member.role, member.joinedUnix);
    activity->storedPhase = activity->phase;
    storedLeaders[id] = activity->leader.GetCounter();

    LOG_INFO(LOGGER, "activity #{} {} started: {} levels {}-{}, {} bots, {}", id, ActivityKindName(activity->kind),
             TeamName(activity->team), uint32(activity->minLevel), uint32(activity->maxLevel), activity->BotsIn(),
             activity->target);
    activities.emplace(id, std::move(activity));
    if (plan.kind == ActivityKindId::Escort && !plan.requester.IsEmpty())
        escortCooldownUntilMs[plan.requester.GetCounter()] =
            nowMs + uint64(config->escortPlayerCooldownMinutes) * MINUTE * IN_MILLISECONDS;
    ++startedCount;
    dirty = true;
    return id;
}

// --- Commands ---

std::vector<std::string> ActivityDirector::List() const
{
    COA_ASSERT_WORLD_THREAD();
    uint64 const nowMs = CoaTime::NowMs();
    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat(
        "CoA activities: {}, scheduler {}, next in {} s, load {} ms (suspend > {}, abort > {}){}",
        Running() ? "running" : "off", paused ? "paused" : "on", SecondsLeft(nextScheduleMs, nowMs), lastLoadMs,
        config->suspendAboveMs, config->abortAboveMs, suspended ? ", SUSPENDED" : ""));
    lines.push_back(Acore::StringFormat("participants {}/{}, real players online {}, plans {}", Participants(),
                                        ParticipantCap(), realPlayers.size(), plans.size()));
    for (auto const& [id, activity] : activities)
    {
        std::string line = Acore::StringFormat(
            "#{} {} {} {} {} s bots {} players {} {}", id, ActivityKindName(activity->kind), TeamName(activity->team),
            ActivityPhaseName(activity->phase), (nowMs - std::min(nowMs, activity->startedMs)) / 1000,
            activity->BotsIn(), activity->PlayersIn(), activity->name);
        if (!activity->target.empty() && activity->target != activity->name)
            line += " [" + activity->target + "]";
        line += Acore::StringFormat(" (ends in {} s)", SecondsLeft(activity->hardDeadlineMs, nowMs));
        lines.push_back(std::move(line));
    }
    ActivityDuelCooldowns const& duels = ActivityDuelCooldowns::Instance();
    lines.push_back(Acore::StringFormat("duels: challenges {}, declines {}, fought vs players {}, brain asked {}",
                                        duels.Challenges(), duels.Declines(), duels.RealDuels(), duels.BrainAsked()));
    auto const ended = [this](ActivityOutcome outcome) { return endedCount[std::size_t(outcome)]; };
    lines.push_back(Acore::StringFormat("ended: success {}, timeout {}, too-few {}, leader-gone {}, load {}, "
                                        "stopped {}, disabled {}, no-players {}, guarded {}",
                                        ended(ActivityOutcome::Success), ended(ActivityOutcome::Timeout),
                                        ended(ActivityOutcome::TooFew), ended(ActivityOutcome::LeaderGone),
                                        ended(ActivityOutcome::LoadAbort), ended(ActivityOutcome::Stopped),
                                        ended(ActivityOutcome::Disabled), ended(ActivityOutcome::NoPlayers),
                                        ended(ActivityOutcome::Guarded)));
    if (config->raidEnable)
    {
        uint32 const nowUnix = uint32(time(nullptr));
        auto const pool = [&](uint8 team)
        {
            std::optional<uint32> const p10 = census.RaidPoolP10(team, nowUnix);
            return p10 ? std::to_string(*p10) : std::string("-");
        };
        uint32 const needs = (config->raidMinBots * ActivityThreshold::RAID_CENSUS_PERCENT + 99) / 100;
        lines.push_back(Acore::StringFormat("raid gate: alliance p10 {}, horde p10 {} (needs {})", pool(TEAM_ALLIANCE),
                                            pool(TEAM_HORDE), needs));
    }
    return lines;
}

std::vector<std::string> ActivityDirector::Info(uint32 id) const
{
    COA_ASSERT_WORLD_THREAD();
    auto const it = activities.find(id);
    if (it == activities.end())
        return {Acore::StringFormat("no activity #{}", id)};

    Activity const& activity = *it->second;
    uint64 const nowMs = CoaTime::NowMs();
    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat("#{} {} {} {} for {} s, started {} s ago, {}, levels {}-{}", id,
                                        ActivityKindName(activity.kind), TeamName(activity.team),
                                        ActivityPhaseName(activity.phase),
                                        (nowMs - std::min(nowMs, activity.phaseSinceMs)) / 1000,
                                        (nowMs - std::min(nowMs, activity.startedMs)) / 1000,
                                        activity.gm ? "gm" : "ambient", uint32(activity.minLevel),
                                        uint32(activity.maxLevel)));
    lines.push_back(Acore::StringFormat("center map {} {:.1f} {:.1f} {:.1f} ({}) radius {:.0f}, travel {}, flags {}",
                                        activity.center.mapId, activity.center.x, activity.center.y, activity.center.z,
                                        EnglishAreaName(activity.center.zoneId), activity.radius,
                                        TravelName(activity.travel), FlagNames(activity.viewFlags)));
    if (ActivityKindIsGroup(activity.kind))
        lines.push_back(Acore::StringFormat("leader {} group {}{}",
                                            activity.leader.IsEmpty() ? std::string("-") : CharacterName(activity.leader),
                                            activity.groupGuid.GetCounter(),
                                            activity.follow.IsEmpty() ? std::string()
                                                                      : " follow " + CharacterName(activity.follow)));

    for (ActivityMember const& member : activity.members)
    {
        Player* const player = ActivityWorld::FindMember(member);
        std::string state = "offline";
        uint32 level = member.level;
        std::string where = "-";
        if (player)
        {
            level = player->GetLevel();
            state = player->IsAlive()
                        ? std::string("alive")
                        : Acore::StringFormat("dead {} s",
                                              member.deadSinceMs ? (nowMs - std::min(nowMs, member.deadSinceMs)) / 1000
                                                                 : 0);
            float const distance = Distance(player, activity.center);
            if (distance != FAR_AWAY)
                where = Acore::StringFormat("{:.0f}", distance);
        }
        std::string line = Acore::StringFormat("{} {} {} lvl {} {} {} yd", CharacterName(member.guid),
                                               member.isBot ? "bot" : "player",
                                               member.role == ActivityRole::Leader ? "leader" : "member", level, state,
                                               where);
        if (member.arrived)
            line += " arrived";
        if (!member.In())
            line += std::string(" left: ") + ActivityLeaveReasonName(member.leftReason);
        lines.push_back(std::move(line));
    }
    if (ActivityKind const* const kind = KindOf(activity.kind))
        lines.push_back(kind->Describe(activity, nowMs));
    return lines;
}

std::vector<std::string> ActivityDirector::VerifyM7(uint32 id)
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<std::string> lines = {"scope: live registry + loaded units; synthetic hook inputs; no real cast"};
    auto const skip = [&lines](std::string why)
    {
        lines.push_back("SKIP M7 verify: " + std::move(why));
        return lines;
    };
    auto const check = [&lines](bool ok, char const* name)
    {
        lines.push_back(std::string(ok ? "PASS " : "FAIL ") + name);
    };
    if (!Running())
        return skip("activities are off");
    auto const found = activities.find(id);
    if (found == activities.end() || found->second->kind != ActivityKindId::RareScout)
        return skip("choose a live RareScout activity id");
    Activity const& activity = *found->second;
    auto const current = ActivityConfig::Current();
    ActivityHuntTarget const* const target = current->FindRareScoutTarget(activity.target);
    if (!target)
        return skip("the activity's target is no longer configured");
    // A reload may rename/repoint an alias while a live kind retains its originating spawn.
    RareScoutState const* const state = dynamic_cast<RareScoutState const*>(activity.state.get());
    if (!state || state->spawn.spawnId != target->spawnId)
        return skip("the configured target differs from the live activity's originating spawn");
    Creature* const rare = ActivityGuards::FindSpawn(target->spawnId);
    if (!rare || !rare->IsAlive() || !rare->IsInWorld() || rare->IsDuringRemoveFromWorld())
        return skip("the configured rare spawn is unloaded or dead");
    uint32 const rank = rare->GetCreatureTemplate()->rank;
    if (rank != CREATURE_ELITE_RARE && rank != CREATURE_ELITE_RAREELITE)
        return skip("the loaded spawn is no longer a rare creature");

    Player* bot = nullptr;
    ActivityMember const* member = nullptr;
    for (ActivityMember const& candidate : activity.members)
        if (candidate.isBot && candidate.In() && Locate(candidate, bot) == Presence::Here)
        {
            member = &candidate;
            break;
        }
    if (!member || !bot || !bot->IsAlive() || bot->GetMap() != rare->GetMap())
        return skip("no alive member with its originating epoch on the rare's loaded map");
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);
    auto const row = ActivityRegistry::Find(bot->GetGUID().GetCounter());
    bool const registered = ai && row && row->activityId == id && row->kind == ActivityKindId::RareScout &&
        row->epoch == member->epoch && ai->GetCoaEpoch() == member->epoch;
    check(registered, "live RareScout registry row and member epoch agree");
    if (!registered)
        return lines;
    check(ai->HasM7ControlledGuard(), "live RareScout member has its enrolled safety guard");
    float const campDistance = CoaActivityBot::DistanceToCenter(bot, *row);
    float const expectedDistance = bot->GetExactDist(row->center.x, row->center.y, row->center.z);
    check(std::isfinite(campDistance) && std::fabs(campDistance - expectedDistance) < 0.01f,
          "live RareScout travel distance includes camp height");
    lines.push_back(Acore::StringFormat("camp distance {:.1f} yd (horizontal {:.1f}, height delta {:.1f})",
        campDistance, bot->GetExactDist2d(row->center.x, row->center.y),
        bot->GetPositionZ() - row->center.z));
    check(!ActivityPvePolicy::AttackAllowed(ai, rare), "actual rare target rejects AttackAllowed");
    uint32 damage = 1;
    ActivityPvePolicy::OnDamage(bot, rare, damage);
    check(damage == 0, "actual bot-to-rare damage input is reduced from one to zero");

    // Spell's constructor only reads caster/metadata and initializes local storage. Without prepare(), scripts,
    // events, references from current casts and effect buffers stay empty; the destructor has no gameplay work.
    SpellInfo const* const fireball = sSpellMgr->GetSpellInfo(133);
    if (!fireball || fireball->IsPositive())
        lines.push_back("SKIP hostile cast and reflection hooks: spell 133 is absent or no longer hostile");
    else
    {
        Spell hostile(bot, fireball, TRIGGERED_FULL_MASK);
        hostile.m_targets.SetUnitTarget(rare);
        check(!ActivityPvePolicy::CanPrepare(&hostile), "policy rejects synthetic hostile rare cast");
        check(!sScriptMgr->CanPrepare(&hostile, &hostile.m_targets, nullptr),
              "installed CanPrepare hook rejects synthetic hostile rare cast");

        Spell reflected(rare, fireball, TRIGGERED_FULL_MASK);
        reflected.m_targets.SetUnitTarget(bot);
        TargetInfo reflectedTarget{};
        reflectedTarget.targetGUID = bot->GetGUID();
        reflectedTarget.missCondition = SPELL_MISS_REFLECT;
        reflectedTarget.reflectResult = SPELL_MISS_NONE;
        reflectedTarget.effectMask = 1;
        reflectedTarget.alive = true;
        reflectedTarget.damage = 1;
        reflectedTarget.damageBeforeTakenMods = 1;
        sScriptMgr->OnSpellCalculatedTarget(&reflected, bot, reflectedTarget);
        check(reflectedTarget.reflectResult == SPELL_MISS_IMMUNE && reflectedTarget.damage == 0 &&
                  reflectedTarget.damageBeforeTakenMods == 0,
              "installed calculated-target hook blocks rare reflection with immune and zero damage");
    }

    SpellInfo const* const heal = sSpellMgr->GetSpellInfo(2050);
    Player* const master = ai->GetMaster();
    bool const safe = ActivityPvePolicy::RareSafe(bot) &&
        (!master || (master->GetMap() == bot->GetMap() && ActivityPvePolicy::SafePlayer(master)));
    if (!heal || !heal->IsPositive())
        lines.push_back("SKIP positive self cast: spell 2050 is absent or no longer positive");
    else if (!safe)
        lines.push_back("SKIP positive self cast: the current bot or master fails the activity safety checks");
    else
    {
        Spell friendly(bot, heal, TRIGGERED_FULL_MASK);
        friendly.m_targets.SetUnitTarget(bot);
        bool const allowed = ActivityPvePolicy::CanPrepare(&friendly);
        check(allowed, "policy accepts synthetic safe positive self cast");
        if (allowed && !sScriptMgr->CanPrepare(&friendly, &friendly.m_targets, nullptr))
            lines.push_back("SKIP installed positive CanPrepare: policy permits the cast but another hook rejects it");
        else if (allowed)
            lines.push_back("PASS installed CanPrepare hook accepts synthetic safe positive self cast");
    }
    return lines;
}

std::vector<std::string> ActivityDirector::Start(ActivityStartRequest request, Player* gmPlayer)
{
    COA_ASSERT_WORLD_THREAD();
    // An escort always belongs to its requesting player. A GM uses the same caps, cooldown and duplicate checks;
    // the console cannot invent an owner or start a GM-exempt escort by forcing team/level/size.
    if (request.kind == ActivityKindId::Escort)
        return RequestEscort(gmPlayer, std::move(request.target));
    if (!Running())
        return {"CoA activities are off"};
    config = ActivityConfig::Current();

    char const* const kindName = ActivityKindName(request.kind);
    if (!KindOf(request.kind))
        return {Acore::StringFormat("unknown activity kind {}", kindName)};
    if (!config->KindEnabled(request.kind))
        return {Acore::StringFormat("{} is disabled (AiPlayerbot.CoaActivity.{}.Enable = 0)", kindName,
                                    KindKey(request.kind))};
    if (LiveCount(request.kind) >= config->KindMax(request.kind))
        return {Acore::StringFormat("{}: {} running, AiPlayerbot.CoaActivity.{}.Max is {}", kindName,
                                    LiveCount(request.kind), KindKey(request.kind), config->KindMax(request.kind))};
    if (activities.size() >= config->maxConcurrent)
        return {Acore::StringFormat("{} activities running, AiPlayerbot.CoaActivity.MaxConcurrent is {}",
                                    activities.size(), config->maxConcurrent)};
    if (suspended)
        return {Acore::StringFormat("server load {} ms is above SuspendAboveMs {}", lastLoadMs,
                                    config->suspendAboveMs)};

    // A command runs between two director ticks: this tick's players, budget and reservations again.
    uint64 const nowMs = CoaTime::NowMs();
    uint32 const nowUnix = uint32(time(nullptr));
    CollectRealPlayers();
    ops.BeginTick(config->maxOpsPerTick, &realPlayers);
    organizer.BeginTick(config.get(), organizerSink, nowMs);
    RebuildHeld();
    candidates.BeginTick(nowMs, &held, &botCooldownUntilMs);

    request.gm = true;
    if (!request.nearPlayer && gmPlayer && gmPlayer->IsInWorld())
        request.nearPlayer = gmPlayer;
    request.capacity = Capacity();
    if (request.capacity < MinBots(request.kind))
        return {Acore::StringFormat("{}: {} participant places left (MaxParticipants, MaxParticipantsPercent), it "
                                    "needs {}",
                                    kindName, request.capacity, MinBots(request.kind))};

    std::string why;
    if (!BeginPlan(request, false, why))
        return {Acore::StringFormat("cannot start {}: {}", kindName, why)};

    // Resolve this plan only (the dice decide now); ambient plans keep waiting for their answers.
    PendingPlan pending = std::move(plans.back());
    plans.pop_back();
    dirty = true;
    uint32 const id = ResolvePlan(pending, nowMs, nowUnix);
    Publish();
    if (!id)
        return {Acore::StringFormat("{}: not enough bots accepted", kindName)};

    Activity const& activity = *activities.at(id);
    return {Acore::StringFormat("started activity #{} {}: {} levels {}-{}, {} bots, {}", id, kindName,
                                TeamName(activity.team), uint32(activity.minLevel), uint32(activity.maxLevel),
                                activity.BotsIn(), activity.target)};
}

std::vector<std::string> ActivityDirector::RequestEscort(Player* player, std::string target)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Running())
        return {ActivityWorld::Line("coa_act_off", "Bot activities are currently off.")};
    config = ActivityConfig::Current();
    if (!config->enable || !config->escortEnable)
        return {ActivityWorld::Line("coa_act_escort_off", "Escorts are currently unavailable.")};
    if (!player || !player->GetSession() || player->GetSession()->IsBot() || !player->IsInWorld() ||
        player->IsDuringRemoveFromWorld())
        return {ActivityWorld::Line("coa_act_escort_player", "An online player must request the escort.")};

    uint64 const nowMs = CoaTime::NowMs();
    uint32 const nowUnix = uint32(time(nullptr));
    uint32 const playerLow = player->GetGUID().GetCounter();
    PruneEscortRequests(nowMs);
    auto const last = escortRequestAtMs.find(playerLow);
    if (last != escortRequestAtMs.end() && ActivityRequestRules::RequestThrottled(last->second, nowMs))
        return {ActivityWorld::Line("coa_act_escort_gap", "Please wait five seconds before requesting again.")};
    escortRequestAtMs[playerLow] = nowMs;

    for (auto const& [id, activity] : activities)
        if (ActivityRequestRules::OwnsEscort(activity->kind, activity->requester.GetRawValue(),
                                            player->GetGUID().GetRawValue()))
            return {ActivityWorld::Line("coa_act_escort_already", "You already have an escort; cancel it first.")};
    for (PendingPlan const& pending : plans)
        if (ActivityRequestRules::OwnsEscort(pending.plan.kind, pending.plan.requester.GetRawValue(),
                                            player->GetGUID().GetRawValue()))
            return {ActivityWorld::Line("coa_act_escort_already", "You already have an escort; cancel it first.")};
    auto const cooldown = escortCooldownUntilMs.find(playerLow);
    if (cooldown != escortCooldownUntilMs.end() && nowMs < cooldown->second)
        return {ActivityWorld::Line("coa_act_escort_cooldown", "You must wait before requesting another escort.")};

    ActivitySpot const* const spot = config->FindEscortSpot(target);
    if (!spot)
        return {ActivityWorld::Line("coa_act_escort_destination", "Choose one of the configured escort destinations.")};
    uint32 pendingEscorts = 0;
    for (PendingPlan const& pending : plans)
        pendingEscorts += pending.plan.kind == ActivityKindId::Escort ? 1 : 0;
    if (LiveCount(ActivityKindId::Escort) + pendingEscorts >= config->escortMax ||
        activities.size() + plans.size() >= config->maxConcurrent)
        return {ActivityWorld::Line("coa_act_escort_busy", "All escort places are currently occupied.")};

    // A player command runs between world ticks. Refresh the same world-thread services as GM Start, with all
    // ordinary-player caps retained and no GM override; the kind validates the requester and destination again.
    CollectRealPlayers();
    ops.BeginTick(config->maxOpsPerTick, &realPlayers);
    organizer.BeginTick(config.get(), organizerSink, nowMs);
    RebuildHeld();
    candidates.BeginTick(nowMs, &held, &botCooldownUntilMs);
    LoadGuard(nowMs);
    if (suspended)
        return {ActivityWorld::Line("coa_act_escort_load", "The escorts are resting while the realm is busy.")};
    ActivityStartRequest request;
    request.kind = ActivityKindId::Escort;
    request.nearPlayer = player;
    request.target = spot->name;
    request.team = uint8(player->GetTeamId());
    request.capacity = Capacity();
    if (request.capacity < MinBots(request.kind))
        return {ActivityWorld::Line("coa_act_escort_busy", "All escort places are currently occupied.")};
    std::string why;
    if (!BeginPlan(request, false, why))
    {
        RebuildHeld();
        return {ActivityWorld::Line("coa_act_escort_no", "No suitable escort can start here right now.")};
    }

    // Resolve only this request. Ambient plans retain their reservations, brain results and deadlines.
    PendingPlan pending = std::move(plans.back());
    plans.pop_back();
    dirty = true;
    uint32 const id = ResolvePlan(pending, nowMs, nowUnix);
    RebuildHeld();
    Publish();
    if (!id)
        return {ActivityWorld::Line("coa_act_escort_no", "No suitable escort can start here right now.")};
    return {ActivityWorld::Line("coa_act_escort_requested", "Your escort is gathering and will invite you shortly.")};
}

std::vector<std::string> ActivityDirector::CancelEscort(Player* player)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Running() || !player || !player->GetSession() || player->GetSession()->IsBot())
        return {ActivityWorld::Line("coa_act_escort_none", "You have no escort to cancel.")};
    config = ActivityConfig::Current();
    CollectRealPlayers();
    ops.BeginTick(config->maxOpsPerTick, &realPlayers);
    organizer.BeginTick(config.get(), organizerSink, CoaTime::NowMs());

    bool found = false;
    for (auto it = plans.begin(); it != plans.end();)
    {
        if (ActivityRequestRules::OwnsEscort(it->plan.kind, it->plan.requester.GetRawValue(),
                                            player->GetGUID().GetRawValue()))
        {
            if (it->jobId && !it->answered)
                BrainService::Current()->Cancel(it->jobId);
            it = plans.erase(it);
            found = true;
            dirty = true;
        }
        else
            ++it;
    }
    std::vector<uint32> owned;
    for (auto const& [id, activity] : activities)
        if (ActivityRequestRules::OwnsEscort(activity->kind, activity->requester.GetRawValue(),
                                            player->GetGUID().GetRawValue()))
            owned.push_back(id);
    uint32 const nowUnix = uint32(time(nullptr));
    for (uint32 const id : owned)
    {
        Activity& activity = *activities.at(id);
        activity.Finish(ActivityOutcome::Stopped, "cancelled by its requesting player");
        EndActivity(activity, ActivityOutcome::Stopped, nowUnix);
        found = true;
    }
    if (found)
    {
        RebuildHeld();
        Publish();
    }
    return {found ? ActivityWorld::Line("coa_act_escort_cancelled", "Your escort has ended.")
                  : ActivityWorld::Line("coa_act_escort_none", "You have no escort to cancel.")};
}

std::vector<std::string> ActivityDirector::EscortDestinations(Player* player) const
{
    COA_ASSERT_WORLD_THREAD();
    std::shared_ptr<ActivityConfig const> const current = ActivityConfig::Current();
    if (!Running() || !current->enable || !current->escortEnable)
        return {ActivityWorld::Line("coa_act_escort_off", "Escorts are currently unavailable.")};
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || !player->GetSession() ||
        player->GetSession()->IsBot())
        return {ActivityWorld::Line("coa_act_escort_player", "An online player must request the escort.")};
    std::string destinations;
    uint32 count = 0;
    for (ActivitySpot const& spot : current->escortSpots)
    {
        if (spot.mapId != player->GetMapId() || (spot.team != TEAM_NEUTRAL && spot.team != player->GetTeamId()))
            continue;
        std::string const next = (destinations.empty() ? "" : ", ") + spot.name;
        if (count >= 16 || destinations.size() + next.size() > 350)
            break;
        destinations += next;
        ++count;
    }
    if (destinations.empty())
        return {ActivityWorld::Line("coa_act_escort_no_destinations", "No escort destinations are configured here.")};
    std::string line = ActivityWorld::Line("coa_act_escort_destinations", "Escort destinations: %destinations",
                                          {{"%destinations", destinations}});
    CoaText::CutUtf8Bytes(line, 384);
    return {std::move(line)};
}

std::vector<std::string> ActivityDirector::Stop(uint32 id)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Running())
        return {"CoA activities are off"};

    uint32 const nowUnix = uint32(time(nullptr));
    CollectRealPlayers();
    ops.BeginTick(config->maxOpsPerTick, &realPlayers);
    organizer.BeginTick(config.get(), organizerSink, CoaTime::NowMs());

    if (!id)
    {
        std::vector<uint32> ids;
        for (auto const& [activityId, activity] : activities)
            ids.push_back(activityId);
        for (uint32 const activityId : ids)
        {
            Activity& activity = *activities.at(activityId);
            activity.Finish(ActivityOutcome::Stopped);
            EndActivity(activity, ActivityOutcome::Stopped, nowUnix);
        }
        return {Acore::StringFormat("stopped {} activities", ids.size())};
    }

    auto const it = activities.find(id);
    if (it == activities.end())
        return {Acore::StringFormat("no activity #{}", id)};
    it->second->Finish(ActivityOutcome::Stopped);
    EndActivity(*it->second, ActivityOutcome::Stopped, nowUnix);
    return {Acore::StringFormat("stopped activity #{}", id)};
}

std::string ActivityDirector::Pause()
{
    COA_ASSERT_WORLD_THREAD();
    paused = true;
    return "scheduler paused (running activities go on)";
}

std::string ActivityDirector::Resume()
{
    COA_ASSERT_WORLD_THREAD();
    paused = false;
    return "scheduler resumed";
}

std::vector<std::string> ActivityDirector::Census(uint32 hours) const
{
    COA_ASSERT_WORLD_THREAD();
    return census.Report(uint32(time(nullptr)), hours);
}

std::string ActivityDirector::Join(Player* player, uint32 id)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Running() || !player)
        return JoinLine("coa_act_off", "Bot activities are off on this realm.");
    std::string const none = JoinLine("coa_act_join_none", "No bot activity you can join right now.");
    if (!player->IsInWorld())
        return none;

    Activity* const target = JoinTarget(player, id);
    if (!target)
        return none;
    // This tick's players again (the kind may check who sees what); the operation budget is the tick's.
    CollectRealPlayers();
    organizer.BeginTick(config.get(), organizerSink, CoaTime::NowMs());
    return JoinChecked(player, *target);
}

Activity* ActivityDirector::JoinTarget(Player* player, uint32 id)
{
    if (id)
    {
        auto const it = activities.find(id);
        return it == activities.end() ? nullptr : it->second.get();
    }

    // The nearest one this player may join: group kinds first, then distance.
    uint32 const level = player->GetLevel();
    Activity* target = nullptr;
    float best = FAR_AWAY;
    bool bestGroup = false;
    for (auto const& [activityId, activity] : activities)
    {
        if (activity->finishing || activity->phase == ActivityPhase::Resolving ||
            activity->team != uint8(player->GetTeamId()) || !activity->center.set ||
            activity->center.mapId != player->GetMapId() || level + JOIN_LEVEL_MARGIN < activity->minLevel ||
            level > uint32(activity->maxLevel) + JOIN_LEVEL_MARGIN)
            continue;
        bool const group = ActivityKindIsGroup(activity->kind);
        float const distance = Distance(player, activity->center);
        if (!target || (group && !bestGroup) || (group == bestGroup && distance < best))
        {
            target = activity.get();
            best = distance;
            bestGroup = group;
        }
    }
    return target;
}

std::string ActivityDirector::JoinChecked(Player* player, Activity& activity)
{
    std::string const none = JoinLine("coa_act_join_none", "No bot activity you can join right now.");
    uint32 const level = player->GetLevel();
    if (ActivityMember const* const member = activity.FindMember(player->GetGUID()); member && member->In())
        return JoinLine("coa_act_join_already", "You are already in that activity.");
    if (activity.team != uint8(player->GetTeamId()))
        return JoinLine("coa_act_join_faction", "That activity is for the other faction.");
    if (level + JOIN_LEVEL_MARGIN < activity.minLevel || level > uint32(activity.maxLevel) + JOIN_LEVEL_MARGIN)
        return JoinLine("coa_act_join_level", "That activity is for levels %min to %max.",
                        {{"%min", std::to_string(uint32(activity.minLevel))},
                         {"%max", std::to_string(uint32(activity.maxLevel))}});

    bool const groupKind = ActivityKindIsGroup(activity.kind);
    Group* const group =
        activity.groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
    if (groupKind && player->GetGroup())
    {
        if (group && player->GetGroup() == group)
            return JoinLine("coa_act_join_already", "You are already in that activity.");
        return JoinLine("coa_act_join_grouped", "Leave your group first.");
    }
    if (groupKind && group && group->GetMembersCount() >= RAID_MEMBERS_MAX)
        return JoinLine("coa_act_join_full", "That group is full.");
    if (activity.finishing || activity.phase == ActivityPhase::Resolving || suspended)
        return none;

    ActivityKind* const kind = KindOf(activity.kind);
    if (!kind)
        return none;
    ActivityServices services = Services(CoaTime::NowMs(), uint32(time(nullptr)));
    std::string line = kind->Join(services, activity, player);
    if (activity.viewDirty || activity.membershipDirty)
        dirty = true;
    return line;
}

// --- Chat recruitment (M5) ---

void ActivityDirector::JoinRequests(uint64 nowMs, uint32 nowUnix)
{
    std::vector<ActivityJoinRequest> requests = std::move(heldRequests);
    heldRequests.clear();
    ActivityRecruitment::Drain(requests);
    for (ActivityJoinRequest const& request : requests)
    {
        if (JoinFromChat(request, nowMs, nowUnix))
            continue;
        if (nowMs < request.atMs + JOIN_REQUEST_HOLD_MS)
            heldRequests.push_back(request);
        else
            LOG_DEBUG(LOGGER, "activity chat join: request of player {} dropped (no budget for 10 s)",
                      request.player.GetCounter());
    }
}

bool ActivityDirector::JoinFromChat(ActivityJoinRequest const& request, uint64 nowMs, uint32 nowUnix)
{
    Player* const player = ObjectAccessor::FindConnectedPlayer(request.player);
    if (!player || !player->IsInWorld() || !player->GetSession() || player->GetSession()->IsBot())
        return true;
    // An answer may take an invitation and a whisper (or two offers): short of budget, the request waits for the
    // next tick before anything happened.
    if (ops.Used() + std::min(JOIN_REQUEST_OPS, config->maxOpsPerTick) > config->maxOpsPerTick)
        return false;

    uint32 const level = player->GetLevel();
    if (request.source == ActivityJoinSource::LfgRequest)
    {
        // "lfg bot ...": recruiting leaders that would take this player whisper an offer, once per activity.
        if (player->GetGroup() || suspended)
            return true;
        uint32 offered = 0;
        for (auto const& [id, owned] : activities)
        {
            if (offered >= ActivityThreshold::RECRUIT_OFFERS_PER_REQUEST)
                break;
            Activity& activity = *owned;
            if (activity.finishing || !(activity.viewFlags & ACTIVITY_VIEW_RECRUITING) ||
                activity.team != uint8(player->GetTeamId()) || level + JOIN_LEVEL_MARGIN < activity.minLevel ||
                level > uint32(activity.maxLevel) + JOIN_LEVEL_MARGIN ||
                recruitOffers.count({id, request.player}))
                continue;
            ActivityKind* const kind = KindOf(activity.kind);
            if (!kind)
                continue;
            ActivityServices services = Services(nowMs, nowUnix);
            if (!kind->Accepts(services, activity, player))
                continue;
            ActivityMember const* const leaderMember = activity.Leader();
            Player* const leader =
                leaderMember && leaderMember->In() ? ActivityWorld::FindMember(*leaderMember) : nullptr;
            if (!leader)
                continue;
            std::string const line = JoinLine("coa_act_recruit_offer",
                                              "We have a group for %target and room for you. Whisper me inv.",
                                              {{"%target", activity.name}});
            ActivityOpResult const result =
                ops.PostToChannel(leader->GetGUID(), ActivityChatTarget::Whisper, line, request.player);
            if (result == ActivityOpResult::Done)
            {
                recruitOffers.insert({id, request.player});
                ++offered;
                LOG_DEBUG(LOGGER, "activity #{}: offer whispered to player {}", id, request.player.GetCounter());
            }
            else if (result == ActivityOpResult::Deferred)
                return false;
        }
        return true;
    }

    // A join word to a recruiting leader: that activity, while it still recruits. "!join [id]": that activity, or
    // the nearest one the player may join.
    Activity* activity = nullptr;
    if (request.source == ActivityJoinSource::Keyword)
    {
        auto const it = activities.find(request.activityId);
        if (it != activities.end() && !it->second->finishing && (it->second->viewFlags & ACTIVITY_VIEW_RECRUITING))
            activity = it->second.get();
    }
    else
        activity = JoinTarget(player, request.activityId);

    std::string const line = activity ? JoinChecked(player, *activity)
                                      : JoinLine("coa_act_join_none", "No bot activity you can join right now.");

    // The bot whispered to answers; else the activity's leader.
    Player* speaker = ObjectAccessor::FindConnectedPlayer(request.bot);
    if (!speaker || !speaker->IsInWorld() || !speaker->GetSession() || !speaker->GetSession()->IsBot() ||
        !GET_PLAYERBOT_AI(speaker))
    {
        speaker = nullptr;
        if (activity)
            if (ActivityMember const* const leaderMember = activity->Leader(); leaderMember && leaderMember->In())
                speaker = ActivityWorld::FindMember(*leaderMember);
    }
    LOG_DEBUG(LOGGER, "activity chat join: player {} ({}) -> #{}", request.player.GetCounter(),
              request.source == ActivityJoinSource::Keyword ? "join word" : "prefix", activity ? activity->id : 0);
    if (!speaker)
        return true;
    ActivityOpResult const result =
        ops.PostToChannel(speaker->GetGUID(), ActivityChatTarget::Whisper, line, request.player);
    if (result != ActivityOpResult::Done)
        LOG_DEBUG(LOGGER, "activity chat join: answer to player {} not whispered ({})", request.player.GetCounter(),
                  ActivityOpResultName(result));
    return true;
}
