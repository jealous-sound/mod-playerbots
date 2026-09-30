/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityDirector.h"
#include "ActivityConfig.h"
#include "ActivityDuelCooldowns.h"
#include "ActivityRegistry.h"
#include "BrainService.h"
#include "BrainSink.h"
#include "CharacterCache.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "DBCStores.h"
#include "DuelGathering.h"
#include "GangParty.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
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

bool IsGroupKind(ActivityKindId kind) { return kind == ActivityKindId::Gang; }

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
        {ACTIVITY_VIEW_HUNT, "hunt"}};
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
    return ActivityServices{*config, ops, candidates, ActivityDuelCooldowns::Instance(), realPlayers, nowMs, nowUnix};
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
    for (auto& [id, activity] : activities)
        if (!config->KindEnabled(activity->kind))
            activity->Finish(ActivityOutcome::Disabled, std::string(ActivityKindName(activity->kind)) + " disabled");
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
    LOG_INFO(LOGGER, "CoA activities on: duel {}, gang {}, {} duel spots", config->duelEnable ? "on" : "off",
             config->gangEnable ? "on" : "off", config->duelSpots.size());
    LOG_DEBUG(LOGGER, "CoA activity director started from {}", fromReload ? "a config reload" : "startup");
}

void ActivityDirector::StopRunning(ActivityOutcome outcome)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Running())
        return;

    uint32 const nowUnix = uint32(time(nullptr));
    CollectRealPlayers();
    ops.BeginTick(config->maxOpsPerTick, &realPlayers);

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

    held.clear();
    dirty = true;
    Publish();
    // Bots back in the world since the last tick get their ruleset now; the others at their next login (Restoring()
    // keeps Update coming while activities are off).
    RestorePending();

    ActivityDuelCooldowns::Instance().Clear();
    ActivityStore::Instance().Reset();
    botCooldownUntilMs.clear();
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
    RebuildHeld();
    candidates.BeginTick(nowMs, &held, &botCooldownUntilMs);
    store.ProcessCallbacks();

    LoadGuard(nowMs);

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
            schedule.choice = ActivityBrain::ReadNextKind(result, schedule.eligible);
            continue;
        }
        for (PendingPlan& pending : plans)
        {
            if (pending.planId != result.tag || pending.answered)
                continue;
            std::vector<int8> const answers = ActivityBrain::ReadJoin(result, pending.asked.size());
            for (std::size_t i = 0; i < answers.size(); ++i)
                if (answers[i] >= 0 && pending.asked[i] < pending.plan.decisions.size())
                    pending.plan.decisions[pending.asked[i]] = answers[i];
            pending.answered = true;
            LOG_DEBUG(LOGGER, "activity plan {}: join answers in ({})", pending.planId, BrainErrorName(result.error));
            break;
        }
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
    bool const groupKind = IsGroupKind(activity.kind);
    bool const formed = groupKind && !activity.groupGuid.IsEmpty();
    if (formed)
        groupSinceMs.emplace(activity.id, nowMs);

    Player* leader = nullptr;
    if (ActivityMember* const leaderMember = activity.Leader(); leaderMember && leaderMember->In())
        Locate(*leaderMember, leader);

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

        // The leader leads nobody's follower; followers follow the leader, whoever else joined or relogged.
        if (inGroup)
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
        }

        // Revive rule (the manager's own revival skips participants).
        if (!player->IsAlive())
        {
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
    if (activity.phase != ActivityPhase::Marching)
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
    for (auto it = activity.invites.begin(); it != activity.invites.end();)
    {
        Player* const player = ObjectAccessor::FindConnectedPlayer(it->first);
        if (player && player->IsInWorld() && group && group->IsMember(it->first))
        {
            ActivityMember const* const existing = activity.FindMember(it->first);
            if (!existing || !existing->In())
            {
                activity.Enroll(it->first, 0, false, player->GetLevel(), nowUnix);
                ActivityStore::Instance().InsertMember(activity.id, it->first.GetCounter(), false,
                                                       ActivityRole::Member, nowUnix);
                LOG_DEBUG(LOGGER, "activity #{}: player {} joined", activity.id, player->GetName());
                if (leader)
                {
                    std::string const line =
                        JoinLine("coa_act_join_where", "Welcome! We are in %zone, come find us.",
                                 {{"%zone", ActivityWorld::AreaName(activity.center.zoneId)}, {"%gang", activity.name}});
                    ops.PostToChannel(leader->GetGUID(), ActivityChatTarget::Whisper, line, it->first);
                }
            }
            it = activity.invites.erase(it);
        }
        else if (!player || nowMs >= it->second)
            it = activity.invites.erase(it);
        else
            ++it;
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
            RestoreRuleset(member);
            // Offline members too: a bot that logged out must not come back into the activity's group.
            if (IsGroupKind(activity.kind) && !activity.groupGuid.IsEmpty())
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
            botCooldownUntilMs[member.guid.GetCounter()] = nowMs + uint64(config->botCooldownMinutes) * 60000;
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

uint32 ActivityDirector::MinBots(ActivityKindId kind) const
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return config->duelMinBots;
        case ActivityKindId::Gang:
            return config->gangMinBots;
        default:
            return 0;
    }
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

void ActivityDirector::Schedule(uint64 nowMs, uint32 /*nowUnix*/)
{
    if (paused || suspended || !plans.empty() || schedule.active || nowMs < nextScheduleMs)
        return;
    nextScheduleMs = nowMs + uint64(config->scheduleSeconds) * 1000 * urand(80, 120) / 100;
    if (config->requireRealPlayerOnline && realPlayers.empty())
        return;

    std::vector<ActivityKindId> const eligible = EligibleKinds();
    if (eligible.empty())
        return;
    Player* const nearPlayer = PickNear();

    if (config->useBrainOrganizer)
    {
        std::shared_ptr<IBrain> const brain = BrainService::Current();
        if (brain->Active(BrainUse::Organizer))
        {
            uint64 const tag = nextPlanId++;
            uint64 const deadline = nowMs + config->brainDeadlineMs;
            BrainTicket const ticket =
                brain->Submit(ActivityBrain::BuildNextKindJob(SceneNow(), eligible, tag, deadline), organizerSink,
                              nullptr);
            if (ticket.status == BrainSubmit::Queued)
            {
                schedule = PendingSchedule();
                schedule.tag = tag;
                schedule.jobId = ticket.jobId;
                schedule.deadlineMs = deadline;
                schedule.eligible = eligible;
                schedule.nearGuid = nearPlayer ? nearPlayer->GetGUID() : ObjectGuid::Empty;
                schedule.active = true;
                LOG_DEBUG(LOGGER, "activity schedule: asked the brain ({} kinds eligible)", eligible.size());
                return;
            }
            LOG_DEBUG(LOGGER, "activity schedule: brain {}, dice", BrainSubmitName(ticket.status));
        }
    }
    DicePlan(eligible, nearPlayer);
}

void ActivityDirector::ResolveSchedule(uint64 nowMs, uint32 /*nowUnix*/)
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

    std::vector<ActivityKindId> const eligible = EligibleKinds();
    if (eligible.empty())
        return;

    if (done.answered && done.choice.valid)
    {
        if (done.choice.none)
        {
            LOG_DEBUG(LOGGER, "activity schedule: the brain chose nothing");
            return;
        }
        if (std::find(eligible.begin(), eligible.end(), done.choice.kind) != eligible.end())
        {
            ActivityStartRequest request;
            request.kind = done.choice.kind;
            request.nearPlayer = nearPlayer;
            request.capacity = Capacity();
            std::string why;
            if (!BeginPlan(request, config->useBrainOrganizer, why))
                LOG_DEBUG(LOGGER, "activity schedule: {} not planned: {}", ActivityKindName(request.kind), why);
            return;
        }
    }
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
    lines.push_back(Acore::StringFormat(
        "ended: success {}, timeout {}, too-few {}, leader-gone {}, load {}, stopped {}, disabled {}, no-players {}",
        ended(ActivityOutcome::Success), ended(ActivityOutcome::Timeout), ended(ActivityOutcome::TooFew),
        ended(ActivityOutcome::LeaderGone), ended(ActivityOutcome::LoadAbort), ended(ActivityOutcome::Stopped),
        ended(ActivityOutcome::Disabled), ended(ActivityOutcome::NoPlayers)));
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
    if (IsGroupKind(activity.kind))
        lines.push_back(Acore::StringFormat("leader {} group {}",
                                            activity.leader.IsEmpty() ? std::string("-") : CharacterName(activity.leader),
                                            activity.groupGuid.GetCounter()));

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

std::vector<std::string> ActivityDirector::Start(ActivityStartRequest request, Player* gmPlayer)
{
    COA_ASSERT_WORLD_THREAD();
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

std::vector<std::string> ActivityDirector::Stop(uint32 id)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Running())
        return {"CoA activities are off"};

    uint32 const nowUnix = uint32(time(nullptr));
    CollectRealPlayers();
    ops.BeginTick(config->maxOpsPerTick, &realPlayers);

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

    uint32 const level = player->GetLevel();
    auto const levelFits = [level](Activity const& activity)
    {
        return level + JOIN_LEVEL_MARGIN >= activity.minLevel && level <= uint32(activity.maxLevel) + JOIN_LEVEL_MARGIN;
    };

    Activity* target = nullptr;
    if (id)
    {
        auto const it = activities.find(id);
        if (it == activities.end())
            return none;
        target = it->second.get();
    }
    else
    {
        // The nearest one this player may join: group kinds first, then distance.
        float best = FAR_AWAY;
        bool bestGroup = false;
        for (auto const& [activityId, activity] : activities)
        {
            if (activity->finishing || activity->phase == ActivityPhase::Resolving ||
                activity->team != uint8(player->GetTeamId()) || !activity->center.set ||
                activity->center.mapId != player->GetMapId() || !levelFits(*activity))
                continue;
            bool const group = IsGroupKind(activity->kind);
            float const distance = Distance(player, activity->center);
            if (!target || (group && !bestGroup) || (group == bestGroup && distance < best))
            {
                target = activity.get();
                best = distance;
                bestGroup = group;
            }
        }
        if (!target)
            return none;
    }

    Activity& activity = *target;
    if (ActivityMember const* const member = activity.FindMember(player->GetGUID()); member && member->In())
        return JoinLine("coa_act_join_already", "You are already in that activity.");
    if (activity.team != uint8(player->GetTeamId()))
        return JoinLine("coa_act_join_faction", "That activity is for the other faction.");
    if (!levelFits(activity))
        return JoinLine("coa_act_join_level", "That activity is for levels %min to %max.",
                        {{"%min", std::to_string(uint32(activity.minLevel))},
                         {"%max", std::to_string(uint32(activity.maxLevel))}});

    bool const groupKind = IsGroupKind(activity.kind);
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
    // This tick's players again (the kind may teleport or check who sees what); the operation budget is the tick's.
    CollectRealPlayers();
    ActivityServices services = Services(CoaTime::NowMs(), uint32(time(nullptr)));
    std::string line = kind->Join(services, activity, player);
    if (activity.viewDirty || activity.membershipDirty)
        dirty = true;
    return line;
}
