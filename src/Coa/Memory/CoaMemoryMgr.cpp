/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaMemoryMgr.h"
#include "AsyncCallbackProcessor.h"
#include "BotMemory.h"
#include "CharacterCache.h"
#include "CoaMemoryConfig.h"
#include "CoaMemoryRepository.h"
#include "CoaPlayerPrefs.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "MemoryRetrieval.h"
#include "MemoryView.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "QueryCallback.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <sstream>

namespace
{
// World thread only: the loads and the command queries.
QueryCallbackProcessor& Callbacks()
{
    static QueryCallbackProcessor processor;
    return processor;
}

constexpr std::size_t PENDING_MAX = 64;
constexpr uint32 PUBLISH_PER_TICK = 256;
constexpr uint32 LOGINS_PER_TICK = 50;
constexpr std::size_t FLUSH_SKIP_QUEUE = 2000;
constexpr uint64 SWEEP_INTERVAL_MS = 30 * 1000;
constexpr uint64 RETENTION_INTERVAL_MS = 600 * 1000;
constexpr uint64 FIRST_RETENTION_MS = 60 * 1000;
constexpr uint8 SWEEPS_BEFORE_CLOSE = 2;
constexpr uint32 MEETING_GAP = 1800;
constexpr uint32 MET_AGAIN_GAP = 86400;
constexpr uint32 GROUPED_GAP = 1800;
constexpr uint32 BG_GRUDGE_SECONDS = 3600;
constexpr uint32 REVENGE_PAIR_GAP = 1800;
constexpr uint32 REVENGE_BOT_GAP = 600;
constexpr int16 REVENGE_HALVE_GRUDGE = 300;
constexpr uint32 KICK_DISMISSAL_SECONDS = 300;

// CoaMemoryMgr::Running(): written by the world thread (SetRunning), read by hooks on map threads.
std::atomic<bool> runningFlag{false};

uint32 NowUnix() { return uint32(time(nullptr)); }

ObjectGuid PlayerGuid(uint32 low) { return ObjectGuid::Create<HighGuid::Player>(low); }

// The bot's AI of this life, or null.
PlayerbotAI* LiveAi(uint32 guid, uint64 epoch, Player** out = nullptr)
{
    Player* const bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(guid));
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!ai || ai->GetCoaEpoch() != epoch)
        return nullptr;
    if (out)
        *out = bot;
    return ai;
}

int32 Decayed(MemoryRelation const& relation, MemoryAxis axis, MemoryDecay const& decay, uint32 now)
{
    return decay.Apply(relation.axes[std::size_t(axis)], axis, relation.decayedAt, now);
}

MemoryLabel LabelOf(MemoryRelation const& relation, MemoryDecay const& decay, uint32 now)
{
    std::array<int16, MEMORY_AXIS_COUNT> axes{};
    for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
        axes[i] = decay.Apply(relation.axes[i], MemoryAxis(i), relation.decayedAt, now);
    return MemoryLabelOf(axes, relation.counters, relation.lastKilledMeAt, relation.bgGrudgeUntil, now);
}

bool HostileOptedOut(uint32 account)
{
    return account && (CoaPlayerPrefs::Instance().Get(account).activityMask & COA_OPTOUT_HOSTILE);
}

std::string FirstName(std::string const& name) { return name.substr(0, name.find(' ')); }

std::string Levels(uint8 bot, uint8 other) { return Acore::StringFormat("level {} vs {}", uint32(bot), uint32(other)); }

// Where an online bot is, for events that do not carry a place.
void Whereabouts(uint32 guid, uint16& mapId, uint32& zoneId)
{
    if (Player* const bot = ObjectAccessor::FindConnectedPlayer(PlayerGuid(guid)))
    {
        mapId = uint16(bot->GetMapId());
        zoneId = bot->GetZoneId();
    }
}

std::vector<uint32> GroupMembers(ObjectGuid groupGuid)
{
    std::vector<uint32> members;
    if (!groupGuid)
        return members;
    Group* const group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
    if (!group)
        return members;
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
        members.push_back(slot.guid.GetCounter());
    return members;
}

// Erases what is about any of `accounts` from one memory. The number of things removed.
std::size_t ForgetAccountsIn(BotMemory& memory, std::unordered_set<uint32> const& accounts)
{
    if (accounts.size() == 1)
        return memory.ForgetAccount(*accounts.begin());

    std::size_t removed = 0;
    for (auto it = memory.relations.begin(); it != memory.relations.end();)
    {
        if (it->second.otherAccount && accounts.contains(it->second.otherAccount))
        {
            memory.dirty.erase(it->first);
            it = memory.relations.erase(it);
            ++removed;
        }
        else
            ++it;
    }
    auto const about = [&accounts](MemoryEvent const& event)
    { return event.otherAccount && accounts.contains(event.otherAccount); };
    removed += std::erase_if(memory.events, about);
    std::erase_if(memory.unsaved, about);
    return removed;
}
}  // namespace

CoaMemoryMgr& CoaMemoryMgr::Instance()
{
    static CoaMemoryMgr instance;
    return instance;
}

CoaMemoryConfig const& CoaMemoryMgr::Config() const
{
    static CoaMemoryConfig const disabled;
    return config ? *config : disabled;
}

void CoaMemoryMgr::AddQuery(QueryCallback&& query) { Callbacks().AddCallback(std::move(query)); }

bool CoaMemoryMgr::Running() { return runningFlag.load(std::memory_order_relaxed); }

void CoaMemoryMgr::SetRunning(bool on)
{
    running = on;
    runningFlag.store(on, std::memory_order_relaxed);
}

// --- Lifecycle ---

void CoaMemoryMgr::OnStartup()
{
    COA_ASSERT_WORLD_THREAD();
    std::shared_ptr<CoaMemoryConfig const> current = CoaMemoryConfig::Current();
    if (stopped || running || !current->enable)
        return;
    config = std::move(current);

    CoaMemoryRepository& repository = CoaMemoryRepository::Instance();
    if (!repository.CheckTables())
    {
        tables = TablesState::Missing;
        return;
    }
    tables = TablesState::Ok;
    // Normally done by CreateRandomBots before it created a character; once per process.
    if (config->orphanSweepOnStartup && !CoaMemoryRepository::StartupSwept())
        CoaMemoryRepository::SweepOrphansSync();
    Start("bots load their memory at login");
}

void CoaMemoryMgr::OnConfigReload()
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped)
        return;
    // Turned off: stopped now with the snapshot memory ran with (its MaxEventsPerBot for the last trim), so Running()
    // is never true while memory is off. Events already on the bus reach Update once more, which drops them.
    std::shared_ptr<CoaMemoryConfig const> current = CoaMemoryConfig::Current();
    if (!current->enable)
    {
        if (running)
            Stop();
        return;
    }
    config = std::move(current);
    if (running)
        return;

    if (!CoaMemoryRepository::Instance().CheckTables())
    {
        tables = TablesState::Missing;
        return;
    }
    tables = TablesState::Ok;

    // Bots already online never post a BotLogin again: they load over the next ticks, 50 per tick.
    pendingOnlineLogins.clear();
    auto const queue = [this](Player* bot)
    {
        if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot())
            return;
        if (PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot))
            if (ai->GetCoaEpoch())
                pendingOnlineLogins.emplace_back(bot->GetGUID().GetCounter(), ai->GetCoaEpoch());
    };
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        queue(it->second);
    for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* const player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            continue;
        if (PlayerbotMgr* const mgr = GET_PLAYERBOT_MGR(player))
            for (auto it = mgr->GetPlayerBotsBegin(); it != mgr->GetPlayerBotsEnd(); ++it)
                queue(it->second);
    }
    Start(Acore::StringFormat("{} online bots load their memory now", pendingOnlineLogins.size()).c_str());
}

void CoaMemoryMgr::Start(char const* why)
{
    uint64 const nowMs = CoaTime::NowMs();
    SetRunning(true);
    optOutPurgeDone = false;
    nextRoundMs = nowMs + Config().flushIntervalMs;
    nextSweepMs = nowMs + SWEEP_INTERVAL_MS;
    nextRetentionMs = nowMs + FIRST_RETENTION_MS;
    LOG_INFO("playerbots.memory", "CoA memory on: {}", why);
}

void CoaMemoryMgr::Stop()
{
    for (auto& [guid, entry] : entries)
    {
        if (entry.state != EntryState::Ready)
            continue;
        FinalFlush(*entry.memory);
        Deliver(guid, entry.epoch, nullptr);
    }
    entries.clear();
    recentlyClosed.clear();
    backlog.clear();
    flushQueue.clear();
    pendingOnlineLogins.clear();
    SetRunning(false);
    LOG_INFO("playerbots.memory", "CoA memory off");
}

void CoaMemoryMgr::Update(uint32 /*diff*/, std::vector<CoaEvent>& memoryEvents, CoaMemoryPrint const& print)
{
    COA_ASSERT_WORLD_THREAD();
    if (!printer)
        printer = print;
    if (stopped)
    {
        memoryEvents.clear();
        return;
    }

    std::shared_ptr<CoaMemoryConfig const> current = CoaMemoryConfig::Current();
    if (!running || !current->enable || !CoaMemoryRepository::Instance().TablesReady())
    {
        if (running)
            Stop();  // with the snapshot it ran with
        memoryEvents.clear();
        return;
    }
    config = std::move(current);

    uint64 const nowMs = CoaTime::NowMs();
    Callbacks().ProcessReadyCallbacks();

    CoaPlayerPrefs& prefs = CoaPlayerPrefs::Instance();
    if (!optOutPurgeDone && prefs.IsLoaded())
    {
        // Opt-outs made while memory was off (or before this start): forgotten once, here.
        std::vector<uint32> const accounts = prefs.MemoryOptOuts();
        if (!accounts.empty())
        {
            ForgetAccounts(std::unordered_set<uint32>(accounts.begin(), accounts.end()));
            CoaMemoryRepository::Instance().PurgeAccounts(accounts);
            LOG_DEBUG("playerbots.memory", "CoA memory: purged {} opted-out accounts", accounts.size());
        }
        optOutPurgeDone = true;
    }

    for (uint32 n = 0; n < LOGINS_PER_TICK && !pendingOnlineLogins.empty(); ++n)
    {
        auto const [guid, epoch] = pendingOnlineLogins.front();
        pendingOnlineLogins.pop_front();
        Login(guid, epoch);
    }

    // Inputs in order, at most DrainPerTick per tick; the rest waits in the backlog.
    std::size_t const drain = Config().drainPerTick;
    if (backlog.empty() && memoryEvents.size() <= drain)
    {
        for (CoaEvent const& event : memoryEvents)
            Apply(event, 0);
    }
    else
    {
        backlog.insert(backlog.end(), std::make_move_iterator(memoryEvents.begin()),
                       std::make_move_iterator(memoryEvents.end()));
        std::size_t const n = std::min(drain, backlog.size());
        for (std::size_t i = 0; i < n; ++i)
            Apply(backlog[i], 0);
        backlog.erase(backlog.begin(), backlog.begin() + n);
    }
    memoryEvents.clear();

    uint32 const now = NowUnix();
    Publish(now);
    FlushRound(nowMs);

    uint64 const keepMs = Config().flushIntervalMs;
    std::erase_if(recentlyClosed,
                  [nowMs, keepMs](auto const& closed) { return closed.second.closedMs + keepMs <= nowMs; });

    if (nowMs >= nextSweepMs)
    {
        nextSweepMs = nowMs + SWEEP_INTERVAL_MS;
        Sweep();
    }
    if (nowMs >= nextRetentionMs)
    {
        nextRetentionMs = nowMs + RETENTION_INTERVAL_MS;
        Retain(now);
    }
}

void CoaMemoryMgr::Shutdown(std::vector<CoaEvent>& memoryEvents)
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped)
        return;
    if (!running)
    {
        stopped = true;
        memoryEvents.clear();
        return;
    }

    shuttingDown = true;
    for (CoaEvent const& event : backlog)
        Apply(event, 0);
    for (CoaEvent const& event : memoryEvents)
        Apply(event, 0);
    backlog.clear();
    memoryEvents.clear();

    uint32 flushed = 0;
    for (auto& [guid, entry] : entries)
    {
        if (entry.state == EntryState::Ready && (entry.memory->IsDirty() || entry.memory->eventsSinceTrim))
        {
            FinalFlush(*entry.memory);
            ++flushed;
        }
    }
    entries.clear();
    recentlyClosed.clear();
    flushQueue.clear();
    pendingOnlineLogins.clear();
    SetRunning(false);
    stopped = true;
    LOG_INFO("playerbots.memory", "CoA memory off: {} memories written at shutdown", flushed);
}

void CoaMemoryMgr::TakeMemoryEvents(std::vector<CoaEvent>& events, std::vector<CoaEvent>& memoryEvents)
{
    auto kept = events.begin();
    for (auto it = events.begin(); it != events.end(); ++it)
    {
        if (IsCoaMemoryEvent(it->kind))
            memoryEvents.push_back(std::move(*it));
        else
        {
            if (kept != it)
                *kept = std::move(*it);
            ++kept;
        }
    }
    events.erase(kept, events.end());
}

// --- Login, load, logout ---

void CoaMemoryMgr::Login(uint32 guid, uint64 epoch)
{
    if (!running || shuttingDown || !guid || !epoch)
        return;

    uint32 const account = sCharacterCache->GetCharacterAccountIdByGuid(PlayerGuid(guid));
    if (Config().randomBotsOnly && !sPlayerbotAIConfig.IsInRandomAccountList(account))
        return;

    auto const found = entries.find(guid);
    if (found != entries.end())
    {
        if (found->second.epoch >= epoch)
            return;  // this life, or a login of an older life arriving late
        if (found->second.state == EntryState::Ready)
            Close(guid);
        else
        {
            entries.erase(found);
            ++stats.loadsDropped;
        }
    }

    Entry& entry = entries[guid];
    entry.epoch = epoch;

    auto const closed = recentlyClosed.find(guid);
    if (closed != recentlyClosed.end())
    {
        // A relog within one flush interval: the memory was flushed at logout and is taken back as it is.
        entry.state = EntryState::Ready;
        entry.memory = std::move(closed->second.memory);
        entry.viewDirty = true;
        recentlyClosed.erase(closed);
        ++stats.takenBack;
        LOG_DEBUG("playerbots.memory", "CoA memory: bot {} took its memory back", guid);
        return;
    }

    entry.state = EntryState::Loading;
    StartLoad(guid, entry);
}

void CoaMemoryMgr::StartLoad(uint32 guid, Entry& entry)
{
    struct LoadState
    {
        QueryResult relations;
    };

    uint64 const epoch = entry.epoch;
    uint32 const loadId = ++nextLoadId;
    entry.loadId = loadId;
    auto const state = std::make_shared<LoadState>();
    uint32 const maxEvents = Config().maxEventsLoaded;
    ++stats.loads;

    // Both callbacks run on the world thread (ProcessReadyCallbacks) and look the entry up again.
    AddQuery(
        CoaMemoryRepository::Instance()
            .LoadRelations(guid, Config().maxRelationsPerBot)
            .WithChainingCallback(
                [guid, epoch, loadId, maxEvents, state](QueryCallback& callback, QueryResult relations)
                {
                    CoaMemoryMgr& mgr = CoaMemoryMgr::Instance();
                    auto const found = mgr.entries.find(guid);
                    if (found == mgr.entries.end() || found->second.epoch != epoch || found->second.loadId != loadId ||
                        found->second.state != EntryState::Loading)
                        return;  // ends the chain
                    state->relations = std::move(relations);
                    callback.SetNextQuery(CoaMemoryRepository::Instance().LoadEvents(guid, maxEvents));
                })
            .WithCallback([guid, epoch, loadId, state](QueryResult events)
                          { CoaMemoryMgr::Instance().FinishLoad(guid, epoch, loadId, state->relations, events); }));
}

void CoaMemoryMgr::FinishLoad(uint32 guid, uint64 epoch, uint32 loadId, QueryResult const& relations,
                              QueryResult const& events)
{
    auto const found = entries.find(guid);
    if (!running || found == entries.end() || found->second.epoch != epoch || found->second.loadId != loadId ||
        found->second.state != EntryState::Loading)
        return;

    Entry& entry = found->second;
    auto memory = std::make_shared<BotMemory>(guid);
    CoaMemoryRepository::ReadRelations(relations, *memory);
    CoaMemoryRepository::ReadEvents(events, *memory);

    // Accounts that opted out while this load was in flight (the database purge may run after the SELECT).
    CoaPlayerPrefs& prefs = CoaPlayerPrefs::Instance();
    if (prefs.IsLoaded())
    {
        std::unordered_set<uint32> optedOut;
        for (auto const& [otherGuid, relation] : memory->relations)
            if (relation.otherAccount && prefs.Get(relation.otherAccount).MemoryOptedOut())
                optedOut.insert(relation.otherAccount);
        for (MemoryEvent const& event : memory->events)
            if (event.otherAccount && !optedOut.contains(event.otherAccount) &&
                prefs.Get(event.otherAccount).MemoryOptedOut())
                optedOut.insert(event.otherAccount);
        if (!optedOut.empty())
            ForgetAccountsIn(*memory, optedOut);
    }

    entry.memory = std::move(memory);
    entry.state = EntryState::Ready;
    entry.viewDirty = true;

    std::vector<CoaEvent> pendingEvents = std::move(entry.pendingEvents);
    std::vector<MemoryConversation> pendingNotes = std::move(entry.pendingNotes);
    entry.pendingEvents.clear();
    entry.pendingNotes.clear();
    LOG_DEBUG("playerbots.memory", "CoA memory: bot {} loaded {} relations, {} events, replays {} deeds and {} notes",
              guid, entry.memory->relations.size(), entry.memory->events.size(), pendingEvents.size(),
              pendingNotes.size());

    // `entry` may not survive a replay (a CharacterDeleted of this bot): nothing below touches it.
    for (CoaEvent const& event : pendingEvents)
        Apply(event, guid);
    for (MemoryConversation const& note : pendingNotes)
        ApplyNote(note);
}

void CoaMemoryMgr::RestartLoads(uint32 onlyGuid)
{
    for (auto& [guid, entry] : entries)
        if (entry.state == EntryState::Loading && (!onlyGuid || guid == onlyGuid))
            StartLoad(guid, entry);
}

void CoaMemoryMgr::Logout(uint32 guid, uint64 epoch)
{
    auto const found = entries.find(guid);
    if (found == entries.end() || found->second.epoch != epoch)
        return;

    if (found->second.state == EntryState::Loading)
    {
        entries.erase(found);
        ++stats.loadsDropped;
        return;
    }
    Close(guid);
}

void CoaMemoryMgr::Close(uint32 guid)
{
    auto const found = entries.find(guid);
    if (found == entries.end())
        return;

    if (found->second.state == EntryState::Ready && found->second.memory)
    {
        FinalFlush(*found->second.memory);
        recentlyClosed[guid] = Closed{std::move(found->second.memory), CoaTime::NowMs()};
    }
    entries.erase(found);
}

void CoaMemoryMgr::FinalFlush(BotMemory& memory)
{
    std::size_t const relations = memory.dirty.size();
    std::size_t const events = memory.unsaved.size();
    bool const trimDue = memory.eventsSinceTrim + events > 0;
    if (!CoaMemoryRepository::Instance().Flush(memory, Config().maxEventsPerBot, true))
        return;
    ++stats.finalFlushes;
    stats.flushedRelations += relations;
    stats.flushedEvents += events;
    if (trimDue && !memory.eventsSinceTrim)
        ++stats.trims;
    stats.lastFlushMs = CoaTime::NowMs();
}

// --- Inputs ---

bool CoaMemoryMgr::Identify(ObjectGuid guid, MemoryOther& other) const
{
    if (!guid)
        return false;

    // From the account, as Login decides who keeps a memory: an altbot is a bot session on its owner's account.
    if (Player* const player = ObjectAccessor::FindConnectedPlayer(guid))
    {
        WorldSession* const session = player->GetSession();
        uint32 const account = session ? session->GetAccountId() : 0;
        other.isBot = session && session->IsBot() && sPlayerbotAIConfig.IsInRandomAccountList(account);
        other.account = other.isBot ? 0 : account;
        other.name = player->GetName();
        other.level = uint8(player->GetLevel());
        return true;
    }

    CharacterCacheEntry const* const cached = sCharacterCache->GetCharacterCacheByGuid(guid);
    if (!cached)
        return false;
    other.isBot = sPlayerbotAIConfig.IsInRandomAccountList(cached->AccountId);
    other.account = other.isBot ? 0 : cached->AccountId;
    other.name = cached->Name;
    other.level = cached->Level;
    return true;
}

bool CoaMemoryMgr::OtherAllowed(MemoryOther const& other)
{
    if (other.isBot)
    {
        if (Config().trackBotToBot)
            return true;
        ++stats.droppedBotToBot;
        return false;
    }

    CoaPlayerPrefs& prefs = CoaPlayerPrefs::Instance();
    if (!prefs.IsLoaded())
    {
        ++stats.droppedPrefs;
        return false;
    }
    if (other.account && prefs.Get(other.account).MemoryOptedOut())
    {
        ++stats.droppedOptedOut;
        return false;
    }
    return true;
}

CoaMemoryMgr::Entry* CoaMemoryMgr::OwnerEntry(uint32 owner, CoaEvent const& event, uint32 onlyOwner)
{
    if (!owner || (onlyOwner && owner != onlyOwner))
        return nullptr;

    auto const found = entries.find(owner);
    if (found == entries.end())
        return nullptr;

    ownerSeen = true;
    Entry& entry = found->second;
    if (entry.state == EntryState::Loading)
    {
        if (entry.pendingEvents.size() < PENDING_MAX)
            entry.pendingEvents.push_back(event);
        else
            ++stats.droppedLoadingFull;
        return nullptr;
    }
    return &entry;
}

MemoryRelation& CoaMemoryMgr::Meet(BotMemory& memory, uint32 otherGuid, MemoryOther const& other, uint32 now,
                                   uint16 mapId, uint32 zoneId)
{
    MemoryRelation const* const old = memory.Find(otherGuid);
    bool const isNew = !old;
    uint32 const prevSeen = old ? old->lastSeen : 0;

    MemoryRelation& relation = memory.Touch(otherGuid, other, now, Config().maxRelationsPerBot, Config().decay);
    if (isNew || now >= relation.lastMetAt + MEETING_GAP)
    {
        memory.AddCounter(relation, MemoryCounter::Meetings);
        memory.AddAxis(relation, MemoryAxis::Familiarity, 20);
        if (isNew || now >= prevSeen + MET_AGAIN_GAP)
            Diary(memory, relation, MemoryEventKind::Met, mapId, zoneId, now,
                  isNew ? std::string("first meeting")
                        : Acore::StringFormat("again after {} d", (now - prevSeen) / MET_AGAIN_GAP));
        relation.lastMetAt = now;
    }
    return relation;
}

void CoaMemoryMgr::Diary(BotMemory& memory, MemoryRelation const& relation, MemoryEventKind kind, uint16 mapId,
                         uint32 zoneId, uint32 now, std::string detail)
{
    MemoryEvent event;
    event.otherGuid = relation.otherGuid;
    event.otherAccount = relation.otherIsBot ? 0 : relation.otherAccount;
    event.kind = kind;
    event.importance = uint8(
        std::min<uint32>(MemoryEventBaseImportance(kind) + (relation.otherIsBot ? 0 : MEMORY_IMPORTANCE_REAL_BONUS),
                         MEMORY_IMPORTANCE_MAX));
    event.mapId = mapId;
    event.zoneId = zoneId;
    event.ts = now;
    event.detail = std::move(detail);
    memory.AddEvent(std::move(event), Config().maxEventsLoaded);
}

void CoaMemoryMgr::Apply(CoaEvent const& event, uint32 onlyOwner)
{
    if (!onlyOwner)
        ++stats.applied;

    // A deed none of whose characters keeps a memory (real players, bots without one) counts as no-memory once.
    bool const deed = event.kind != CoaEventKind::BotLogin && event.kind != CoaEventKind::BotLogout &&
                      event.kind != CoaEventKind::CharacterDeleted;
    ownerSeen = false;

    uint32 const now = NowUnix();
    switch (event.kind)
    {
        case CoaEventKind::BotLogin:
            Login(event.sender.GetCounter(), event.epoch);
            break;
        case CoaEventKind::BotLogout:
            Logout(event.sender.GetCounter(), event.epoch);
            break;
        case CoaEventKind::Duel:
            ApplyDuel(event, onlyOwner, now);
            break;
        case CoaEventKind::PlayerKill:
            ApplyPlayerKill(event, onlyOwner, now);
            break;
        case CoaEventKind::BossKill:
            ApplyBossKill(event, onlyOwner, now);
            break;
        case CoaEventKind::CreatureHelp:
            ApplyCreatureHelp(event, onlyOwner, now);
            break;
        case CoaEventKind::GroupJoin:
            ApplyGroupJoin(event, onlyOwner, now);
            break;
        case CoaEventKind::GroupLeave:
            ApplyGroupLeave(event, onlyOwner, now);
            break;
        case CoaEventKind::Trade:
            ApplyTrade(event, onlyOwner, now);
            break;
        case CoaEventKind::CharacterDeleted:
            if (!onlyOwner && event.sender)
            {
                uint32 const guid = event.sender.GetCounter();
                CoaMemoryRepository::Instance().PurgeCharacter(guid);
                entries.erase(guid);
                recentlyClosed.erase(guid);
                ForgetCharacter(guid);
                RestartLoads(0);
                LOG_DEBUG("playerbots.memory", "CoA memory: character {} deleted, forgotten everywhere", guid);
            }
            break;
        default:
            break;
    }

    if (deed && !onlyOwner && !ownerSeen)
        ++stats.droppedNoMemory;
}

void CoaMemoryMgr::ApplyDuel(CoaEvent const& event, uint32 onlyOwner, uint32 now)
{
    bool const fled = event.value == DUEL_FLED;
    uint32 const winner = event.sender.GetCounter();
    uint32 const loser = event.target.GetCounter();
    if (!winner || !loser || winner == loser)
        return;

    if (Entry* const entry = OwnerEntry(winner, event, onlyOwner))
    {
        MemoryOther other;
        if (Identify(event.target, other) && OtherAllowed(other))
        {
            BotMemory& memory = *entry->memory;
            MemoryRelation& relation = Meet(memory, loser, other, now, uint16(event.mapId), event.zoneId);
            memory.AddCounter(relation, MemoryCounter::DuelsWon);
            std::string const detail = Levels(event.senderLevel, event.targetLevel);
            if (!fled)
            {
                memory.AddAxis(relation, MemoryAxis::Rivalry, 80);
                memory.AddAxis(relation, MemoryAxis::Respect, 20);
                memory.AddAxis(relation, MemoryAxis::Familiarity, 20);
                Diary(memory, relation, MemoryEventKind::DuelWon, uint16(event.mapId), event.zoneId, now, detail);
                relation.fledStreak = 0;
            }
            else
            {
                memory.AddAxis(relation, MemoryAxis::Rivalry, 40);
                memory.AddAxis(relation, MemoryAxis::Respect, -30);
                memory.AddAxis(relation, MemoryAxis::Annoyance, 50);
                Diary(memory, relation, MemoryEventKind::DuelFled, uint16(event.mapId), event.zoneId, now, detail);
                if (now < relation.lastFledAt || now - relation.lastFledAt > MemoryThreshold::DUEL_FLED_WINDOW)
                    relation.fledStreak = 0;
                relation.lastFledAt = now;
                if (relation.fledStreak < 255)
                    ++relation.fledStreak;
            }
            entry->viewDirty = true;
        }
    }

    if (Entry* const entry = OwnerEntry(loser, event, onlyOwner))
    {
        MemoryOther other;
        if (Identify(event.sender, other) && OtherAllowed(other))
        {
            BotMemory& memory = *entry->memory;
            MemoryRelation& relation = Meet(memory, winner, other, now, uint16(event.mapId), event.zoneId);
            memory.AddCounter(relation, MemoryCounter::DuelsLost);
            memory.AddAxis(relation, MemoryAxis::Rivalry, fled ? 40 : 80);
            if (!fled)
            {
                memory.AddAxis(relation, MemoryAxis::Respect, 50);
                memory.AddAxis(relation, MemoryAxis::Familiarity, 20);
            }
            std::string detail = Levels(event.targetLevel, event.senderLevel);
            if (fled)
                detail += ", fled";
            Diary(memory, relation, MemoryEventKind::DuelLost, uint16(event.mapId), event.zoneId, now,
                  std::move(detail));
            // A duel they did not flee ends their fleeing streak, as the load rebuilds it.
            relation.fledStreak = 0;
            entry->viewDirty = true;
        }
    }
}

void CoaMemoryMgr::ApplyPlayerKill(CoaEvent const& event, uint32 onlyOwner, uint32 now)
{
    uint32 const killer = event.sender.GetCounter();
    uint32 const victim = event.target.GetCounter();
    if (!killer || !victim || killer == victim)
        return;

    bool const battleground = event.flags & COA_EVENT_BATTLEGROUND;
    uint8 const bgKills = Config().battlegroundKills;
    if (battleground && !bgKills)
        return;
    bool const full = !battleground || bgKills == 2;
    uint16 const mapId = uint16(event.mapId);

    if (Entry* const entry = OwnerEntry(victim, event, onlyOwner))
    {
        MemoryOther other;
        if (Identify(event.sender, other) && OtherAllowed(other))
        {
            BotMemory& memory = *entry->memory;
            MemoryRelation& relation = Meet(memory, killer, other, now, mapId, event.zoneId);
            memory.AddCounter(relation, MemoryCounter::KilledMe);
            if (full)
            {
                uint32 const killerLevel = event.senderLevel;
                uint32 const victimLevel = event.targetLevel;
                memory.AddAxis(relation, MemoryAxis::Grudge, 250);
                memory.AddAxis(relation, MemoryAxis::Affinity, -100);
                memory.AddAxis(relation, MemoryAxis::Rivalry, 100);
                memory.AddAxis(relation, MemoryAxis::Fear, killerLevel >= victimLevel + 5 ? 100 : 30);
                if (killerLevel >= victimLevel + 10)
                {
                    memory.AddAxis(relation, MemoryAxis::Grudge, 100);
                    memory.AddAxis(relation, MemoryAxis::Respect, -100);
                }
                std::string detail = Levels(event.targetLevel, event.senderLevel);
                if (battleground)
                    detail += ", battleground";
                Diary(memory, relation, MemoryEventKind::KilledBy, mapId, event.zoneId, now, std::move(detail));
                relation.lastKilledMeAt = now;
                if (!other.isBot)
                    RevengeCall(victim, *entry, relation, other, event.zoneId, now);
            }
            else
            {
                memory.AddAxis(relation, MemoryAxis::Rivalry, 10);
                relation.bgGrudgeUntil = now + BG_GRUDGE_SECONDS;
            }
            entry->viewDirty = true;
        }
    }

    if (Entry* const entry = OwnerEntry(killer, event, onlyOwner))
    {
        MemoryOther other;
        if (Identify(event.target, other) && OtherAllowed(other))
        {
            BotMemory& memory = *entry->memory;
            MemoryRelation& relation = Meet(memory, victim, other, now, mapId, event.zoneId);
            memory.AddCounter(relation, MemoryCounter::KilledThem);
            if (full)
            {
                memory.AddAxis(relation, MemoryAxis::Rivalry, 50);
                memory.AddAxis(relation, MemoryAxis::Fear, -50);
                std::string detail = Levels(event.senderLevel, event.targetLevel);
                if (battleground)
                    detail += ", battleground";
                int32 const grudge = Decayed(relation, MemoryAxis::Grudge, Config().decay, now);
                if (grudge >= REVENGE_HALVE_GRUDGE)
                {
                    // Revenge taken: the grudge is halved (the stored value, as of decayedAt, likewise).
                    int32 const stored = relation.axes[std::size_t(MemoryAxis::Grudge)];
                    memory.AddAxis(relation, MemoryAxis::Grudge, -(stored - stored / 2));
                    detail += ", revenge";
                }
                Diary(memory, relation, MemoryEventKind::Killed, mapId, event.zoneId, now, std::move(detail));
            }
            else
                memory.AddAxis(relation, MemoryAxis::Rivalry, 10);
            entry->viewDirty = true;
        }
    }
}

void CoaMemoryMgr::RevengeCall(uint32 owner, Entry& entry, MemoryRelation& relation, MemoryOther const& killer,
                               uint32 zoneId, uint32 now)
{
    if (!CoaMemoryConfig::BehaviourActive(MemoryBehaviour::Revenge))
        return;

    Player* bot = nullptr;
    PlayerbotAI* const ai = LiveAi(owner, entry.epoch, &bot);
    if (!ai || !ai->HasStrategy("coa memory", BOT_STATE_NON_COMBAT) || !bot->GetGuildId())
        return;
    if (LabelOf(relation, Config().decay, now) != MemoryLabel::Grudge)
        return;
    BotMemory& memory = *entry.memory;
    if ((relation.lastRevengeCallAt && now < relation.lastRevengeCallAt + REVENGE_PAIR_GAP) ||
        (memory.lastRevengeCallAt && now < memory.lastRevengeCallAt + REVENGE_BOT_GAP))
        return;
    if (HostileOptedOut(killer.account))
        return;

    std::string const text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "coa_mem_revenge_call", "%player just killed me near %zone. Anyone around?",
        {{"%player", FirstName(killer.name)},
         {"%zone", PlayerbotAI::GetLocalizedAreaName(sAreaTableStore.LookupEntry(zoneId))}});
    if (!ai->SayToGuild(text))
        return;
    relation.lastRevengeCallAt = now;
    memory.lastRevengeCallAt = now;
    ++stats.revengeCalls;
    LOG_DEBUG("playerbots.memory", "CoA memory: bot {} called its guild about {}", owner, killer.name);
}

void CoaMemoryMgr::ApplyBossKill(CoaEvent const& event, uint32 onlyOwner, uint32 now)
{
    // Participants: the killer and the members of its group in the same instance.
    std::vector<uint32> participants;
    Player* const killer = ObjectAccessor::FindConnectedPlayer(event.sender);
    if (killer)
        participants.push_back(event.sender.GetCounter());
    for (uint32 const member : GroupMembers(event.groupGuid))
    {
        if (std::find(participants.begin(), participants.end(), member) != participants.end())
            continue;
        Player* const player = ObjectAccessor::FindConnectedPlayer(PlayerGuid(member));
        if (player && player->IsInWorld() && player->GetMapId() == event.mapId &&
            player->GetInstanceId() == event.instanceId)
            participants.push_back(member);
    }
    if (participants.size() < 2)
        return;

    bool const leader = event.flags & COA_EVENT_LEADER;
    MemoryEventKind const kind = leader ? MemoryEventKind::LeaderKill : MemoryEventKind::BossKill;
    std::string const detail = Acore::StringFormat("creature {}", event.entry);

    for (uint32 const owner : participants)
    {
        Entry* const entry = OwnerEntry(owner, event, onlyOwner);
        if (!entry)
            continue;
        BotMemory& memory = *entry->memory;
        for (uint32 const otherGuid : participants)
        {
            if (otherGuid == owner)
                continue;
            MemoryOther other;
            if (!Identify(PlayerGuid(otherGuid), other) || !OtherAllowed(other))
                continue;
            MemoryRelation& relation = Meet(memory, otherGuid, other, now, uint16(event.mapId), event.zoneId);
            memory.AddAxis(relation, MemoryAxis::Camaraderie, leader ? 100 : 60);
            memory.AddAxis(relation, MemoryAxis::Trust, leader ? 50 : 30);
            memory.AddAxis(relation, MemoryAxis::Respect, leader ? 60 : 30);
            Diary(memory, relation, kind, uint16(event.mapId), event.zoneId, now, detail);
            entry->viewDirty = true;
        }
    }
}

void CoaMemoryMgr::ApplyCreatureHelp(CoaEvent const& event, uint32 onlyOwner, uint32 now)
{
    uint32 const killer = event.sender.GetCounter();
    uint32 const recipient = event.target.GetCounter();
    if (!killer || !recipient || killer == recipient)
        return;
    std::string const detail = Acore::StringFormat("creature {}", event.entry);
    uint16 const mapId = uint16(event.mapId);

    // The recipient's side: every memory owner among the recipient and its group, about the killer.
    std::vector<uint32> helped{recipient};
    for (uint32 const member : GroupMembers(event.groupGuid))
        if (member != killer && std::find(helped.begin(), helped.end(), member) == helped.end())
            helped.push_back(member);

    MemoryOther killerSide;
    bool killerKnown = false;
    bool killerChecked = false;
    for (uint32 const owner : helped)
    {
        Entry* const entry = OwnerEntry(owner, event, onlyOwner);
        if (!entry)
            continue;
        if (!killerChecked)
        {
            killerChecked = true;
            killerKnown =
                Identify(event.sender, killerSide) && OtherAllowed(killerSide);
        }
        if (!killerKnown)
            break;
        BotMemory& memory = *entry->memory;
        MemoryRelation& relation = Meet(memory, killer, killerSide, now, mapId, event.zoneId);
        memory.AddCounter(relation, MemoryCounter::HelpedMe);
        memory.AddAxis(relation, MemoryAxis::Gratitude, 150);
        memory.AddAxis(relation, MemoryAxis::Affinity, 40);
        memory.AddAxis(relation, MemoryAxis::Trust, 20);
        memory.AddAxis(relation, MemoryAxis::Familiarity, 10);
        Diary(memory, relation, MemoryEventKind::Helped, mapId, event.zoneId, now, detail);
        entry->viewDirty = true;
    }

    // The killer's side, about the recipient.
    if (Entry* const entry = OwnerEntry(killer, event, onlyOwner))
    {
        MemoryOther other;
        if (Identify(event.target, other) && OtherAllowed(other))
        {
            BotMemory& memory = *entry->memory;
            MemoryRelation& relation = Meet(memory, recipient, other, now, mapId, event.zoneId);
            memory.AddCounter(relation, MemoryCounter::HelpedThem);
            memory.AddAxis(relation, MemoryAxis::Familiarity, 10);
            memory.AddAxis(relation, MemoryAxis::Camaraderie, 10);
            Diary(memory, relation, MemoryEventKind::HelpedThem, mapId, event.zoneId, now, detail);
            entry->viewDirty = true;
        }
    }
}

void CoaMemoryMgr::ApplyGroupJoin(CoaEvent const& event, uint32 onlyOwner, uint32 now)
{
    uint32 const joiner = event.sender.GetCounter();
    std::vector<uint32> const members = GroupMembers(event.groupGuid);
    if (!joiner || members.size() < 2)
        return;

    // groupedSince keeps this group's members only: a pair whose group ended without a kick is forgotten here.
    auto const sameGroup = [&members](Entry& entry)
    {
        std::erase_if(entry.groupedSince, [&members](auto const& pair)
                      { return std::find(members.begin(), members.end(), pair.first) == members.end(); });
    };
    auto const group = [&](Entry& entry, uint32 owner, uint32 otherGuid)
    {
        MemoryOther other;
        if (!Identify(PlayerGuid(otherGuid), other) || !OtherAllowed(other))
            return;
        // Before the dedupe: a quick re-invite starts a new group time.
        entry.groupedSince[otherGuid] = now;
        uint16 mapId = 0;
        uint32 zoneId = 0;
        Whereabouts(owner, mapId, zoneId);
        BotMemory& memory = *entry.memory;
        MemoryRelation& relation = Meet(memory, otherGuid, other, now, mapId, zoneId);
        if (relation.lastGroupedAt && now < relation.lastGroupedAt + GROUPED_GAP)
            return;
        memory.AddCounter(relation, MemoryCounter::GroupsShared);
        memory.AddAxis(relation, MemoryAxis::Camaraderie, 40);
        memory.AddAxis(relation, MemoryAxis::Trust, 20);
        memory.AddAxis(relation, MemoryAxis::Familiarity, 30);
        Diary(memory, relation, MemoryEventKind::Grouped, mapId, zoneId, now, "");
        relation.lastGroupedAt = now;
        entry.viewDirty = true;
    };

    // The joiner about each member, then each member about the joiner.
    if (Entry* const entry = OwnerEntry(joiner, event, onlyOwner))
    {
        sameGroup(*entry);
        for (uint32 const member : members)
            if (member != joiner)
                group(*entry, joiner, member);
    }
    for (uint32 const member : members)
    {
        if (member == joiner)
            continue;
        if (Entry* const entry = OwnerEntry(member, event, onlyOwner))
        {
            sameGroup(*entry);
            group(*entry, member, joiner);
        }
    }
}

void CoaMemoryMgr::ApplyGroupLeave(CoaEvent const& event, uint32 onlyOwner, uint32 now)
{
    if (event.value != GROUP_REMOVEMETHOD_KICK && event.value != GROUP_REMOVEMETHOD_KICK_LFG)
        return;
    uint32 const removed = event.sender.GetCounter();
    uint32 const kicker = event.target.GetCounter();
    if (!removed || !kicker || removed == kicker)
        return;

    Entry* const entry = OwnerEntry(removed, event, onlyOwner);
    if (!entry)
        return;
    // Removed at least KICK_DISMISSAL_SECONDS after grouping with the kicker: the player dismissed the bot after a
    // session (the UI's uninvite is a kick too), which is not held against them. A kick soon after joining, or with
    // no join seen, is hostile. The bot is out of its group: every pair's group time ends.
    auto const since = entry->groupedSince.find(kicker);
    bool const dismissal = since != entry->groupedSince.end() && now >= since->second + KICK_DISMISSAL_SECONDS;
    entry->groupedSince.clear();
    if (dismissal)
        return;
    MemoryOther other;
    if (!Identify(event.target, other) || !OtherAllowed(other))
        return;

    uint16 mapId = 0;
    uint32 zoneId = 0;
    Whereabouts(removed, mapId, zoneId);
    BotMemory& memory = *entry->memory;
    MemoryRelation& relation = Meet(memory, kicker, other, now, mapId, zoneId);
    memory.AddAxis(relation, MemoryAxis::Trust, -150);
    memory.AddAxis(relation, MemoryAxis::Affinity, -80);
    memory.AddAxis(relation, MemoryAxis::Annoyance, 150);
    memory.AddAxis(relation, MemoryAxis::Grudge, 50);
    Diary(memory, relation, MemoryEventKind::KickedBy, mapId, zoneId, now, "kicked");
    entry->viewDirty = true;
}

void CoaMemoryMgr::ApplyTrade(CoaEvent const& event, uint32 onlyOwner, uint32 now)
{
    uint32 const bot = event.sender.GetCounter();
    uint32 const trader = event.target.GetCounter();
    if (!bot || !trader || bot == trader)
        return;

    Entry* const entry = OwnerEntry(bot, event, onlyOwner);
    if (!entry)
        return;
    MemoryOther other;
    if (!Identify(event.target, other) || !OtherAllowed(other))
        return;

    BotMemory& memory = *entry->memory;
    MemoryRelation& relation = Meet(memory, trader, other, now, uint16(event.mapId), event.zoneId);
    memory.AddCounter(relation, MemoryCounter::Trades);
    memory.AddAxis(relation, MemoryAxis::Trust, 50);
    memory.AddAxis(relation, MemoryAxis::Affinity, 20);
    memory.AddAxis(relation, MemoryAxis::Familiarity, 20);
    if (event.value > 0 && event.value2 == 0)
        memory.AddAxis(relation, MemoryAxis::Gratitude, 100);
    Diary(memory, relation, MemoryEventKind::Traded, uint16(event.mapId), event.zoneId, now,
          Acore::StringFormat("received {}, gave {} items", event.value, event.value2));
    entry->viewDirty = true;
}

void CoaMemoryMgr::OnConversation(MemoryConversation const& note)
{
    COA_ASSERT_WORLD_THREAD();
    if (!running || stopped)
        return;

    ++stats.conversations;
    if (note.sentiment.source == MemorySentimentSource::Brain)
        ++stats.brainRated;
    else if (note.sentiment.source == MemorySentimentSource::Heuristic)
        ++stats.heuristic;
    ApplyNote(note);
}

void CoaMemoryMgr::ApplyNote(MemoryConversation const& note)
{
    uint32 const owner = note.bot.GetCounter();
    uint32 const speaker = note.speaker.GetCounter();
    if (!owner || !speaker || owner == speaker)
        return;

    auto const found = entries.find(owner);
    if (found == entries.end())
    {
        ++stats.droppedNoMemory;
        return;
    }

    // The note is a snapshot of a real player's message: the player may be gone by now.
    MemoryOther other;
    other.account = note.speakerAccount;
    other.isBot = false;
    other.name = note.speakerName;
    other.level = note.speakerLevel;
    if (!OtherAllowed(other))
        return;

    Entry& entry = found->second;
    if (entry.state == EntryState::Loading)
    {
        if (entry.pendingNotes.size() < PENDING_MAX)
            entry.pendingNotes.push_back(note);
        else
            ++stats.droppedLoadingFull;
        return;
    }

    uint32 const now = NowUnix();
    BotMemory& memory = *entry.memory;
    MemoryRelation& relation = Meet(memory, speaker, other, now, note.mapId, note.zoneId);

    // Words move affinity by at most DailyChatDeltaCap points per pair per UTC day; the other word-driven axes
    // move while that budget was not spent before this message.
    uint32 const day = now / 86400;
    if (relation.wordDay != day)
    {
        relation.wordDay = day;
        relation.wordUsed = 0;
    }
    int32 const budget = int32(Config().dailyChatDeltaCap) * 10;
    bool const wordsOpen = relation.wordUsed < budget;
    auto const affinityWords = [&](int32 delta)
    {
        int32 const room = std::max(0, budget - int32(relation.wordUsed));
        int32 const moved = std::min(std::abs(delta), room);
        if (!moved)
            return;
        memory.AddAxis(relation, MemoryAxis::Affinity, delta < 0 ? -moved : moved);
        relation.wordUsed = uint16(relation.wordUsed + moved);
    };

    MemorySentiment const& sentiment = note.sentiment;
    float const sc = sentiment.s * sentiment.c;
    std::string const talk = MemoryTalkKindName(note.kind);
    memory.AddAxis(relation, MemoryAxis::Familiarity, 5);
    if (note.kind != MemoryTalkKind::Emote)
    {
        memory.AddCounter(relation, MemoryCounter::Chats);
        affinityWords(int32(std::lround(40.0f * sc)));
        if (sc >= 0.4f)
            Diary(memory, relation, MemoryEventKind::ChatFriendly, note.mapId, note.zoneId, now, talk);
        else if (sc <= -0.4f)
            Diary(memory, relation, MemoryEventKind::ChatHostile, note.mapId, note.zoneId, now, talk);
    }
    else
    {
        affinityWords(int32(std::lround(30.0f * sc)));
        if (sentiment.s < 0.0f && wordsOpen)
            memory.AddAxis(relation, MemoryAxis::Annoyance, 60);
        std::string const detail = Acore::StringFormat("emote {}", note.textEmote);
        if (sentiment.s > 0.0f)
            Diary(memory, relation, MemoryEventKind::EmoteFriendly, note.mapId, note.zoneId, now, detail);
        else if (sentiment.s < 0.0f)
            Diary(memory, relation, MemoryEventKind::EmoteHostile, note.mapId, note.zoneId, now, detail);
    }

    if (sentiment.insult)
    {
        affinityWords(-30);
        if (wordsOpen)
        {
            memory.AddAxis(relation, MemoryAxis::Annoyance, 100);
            memory.AddAxis(relation, MemoryAxis::Grudge, 50);
        }
        Diary(memory, relation, MemoryEventKind::Insulted, note.mapId, note.zoneId, now, talk);
    }
    if (sentiment.thanks)
    {
        affinityWords(20);
        if (wordsOpen)
            memory.AddAxis(relation, MemoryAxis::Gratitude, 30);
        Diary(memory, relation, MemoryEventKind::Thanked, note.mapId, note.zoneId, now, talk);
    }
    entry.viewDirty = true;
}

// --- Forgetting ---

void CoaMemoryMgr::ForgetAccounts(std::unordered_set<uint32> const& accounts)
{
    if (accounts.empty())
        return;
    for (auto& [guid, entry] : entries)
    {
        std::erase_if(entry.pendingNotes,
                      [&accounts](MemoryConversation const& note) { return accounts.contains(note.speakerAccount); });
        if (entry.state == EntryState::Ready && ForgetAccountsIn(*entry.memory, accounts))
            entry.viewDirty = true;
    }
    for (auto& [guid, closed] : recentlyClosed)
        ForgetAccountsIn(*closed.memory, accounts);
}

void CoaMemoryMgr::ForgetCharacter(uint32 guid)
{
    for (auto& [owner, entry] : entries)
    {
        std::erase_if(entry.pendingNotes,
                      [guid](MemoryConversation const& note) { return note.speaker.GetCounter() == guid; });
        if (entry.state == EntryState::Ready && entry.memory->Forget(guid))
            entry.viewDirty = true;
    }
    for (auto& [owner, closed] : recentlyClosed)
        closed.memory->Forget(guid);
}

bool CoaMemoryMgr::PurgeAccount(uint32 accountId)
{
    COA_ASSERT_WORLD_THREAD();
    if (!running || stopped || !accountId)
        return false;

    ForgetAccounts({accountId});
    CoaMemoryRepository::Instance().PurgeAccounts({accountId});
    RestartLoads(0);
    LOG_DEBUG("playerbots.memory", "CoA memory: account {} purged", accountId);
    return true;
}

// --- Publication, flush, sweep, retention ---

std::shared_ptr<MemoryView const> CoaMemoryMgr::BuildView(uint32 guid, Entry const& entry, uint32 now) const
{
    std::vector<MemoryRelationView> relations;
    relations.reserve(entry.memory->relations.size());
    for (auto const& [otherGuid, relation] : entry.memory->relations)
    {
        MemoryRelationView view;
        view.otherGuid = otherGuid;
        view.otherAccount = relation.otherIsBot ? 0 : relation.otherAccount;
        view.otherIsBot = relation.otherIsBot;
        view.fledStreak = relation.fledStreak;
        view.lastFledAt = relation.lastFledAt;
        view.axes = relation.axes;
        view.counters = relation.counters;
        view.firstSeen = relation.firstSeen;
        view.lastSeen = relation.lastSeen;
        view.decayedAt = relation.decayedAt;
        view.lastKilledMeAt = relation.lastKilledMeAt;
        view.bgGrudgeUntil = relation.bgGrudgeUntil;
        relations.push_back(view);
    }
    return std::make_shared<MemoryView const>(guid, entry.epoch, now, Config().decay, std::move(relations));
}

bool CoaMemoryMgr::Deliver(uint32 guid, uint64 epoch, std::shared_ptr<MemoryView const> view)
{
    // Inside CoaWorldScript::OnUpdate the map threads are idle: the swap never races the bot's reads.
    PlayerbotAI* const ai = LiveAi(guid, epoch);
    if (!ai)
        return false;
    ai->SetMemoryView(std::move(view));
    ++stats.viewsPublished;
    return true;
}

void CoaMemoryMgr::Publish(uint32 now)
{
    // Only the behaviours read views: with every switch off they wait (viewDirty stays set) until one is turned on.
    if (!CoaMemoryConfig::AnyBehaviour())
        return;

    uint32 builds = 0;
    for (auto& [guid, entry] : entries)
    {
        if (entry.state != EntryState::Ready)
            continue;
        if (entry.viewDirty && builds < PUBLISH_PER_TICK)
        {
            entry.view = BuildView(guid, entry, now);
            entry.viewDirty = false;
            entry.unpublished = true;
            ++builds;
            ++stats.viewsBuilt;
        }
        if (entry.unpublished && Deliver(guid, entry.epoch, entry.view))
            entry.unpublished = false;
    }
}

void CoaMemoryMgr::FlushRound(uint64 nowMs)
{
    CoaMemoryConfig const& cfg = Config();
    if (nowMs >= nextRoundMs)
    {
        nextRoundMs = nowMs + cfg.flushIntervalMs;
        if (PlayerbotsDatabase.QueueSize() > FLUSH_SKIP_QUEUE)
            ++stats.roundsSkipped;
        else
        {
            // Leftovers of the last round keep their place: a new pass over `entries` starts only once they are
            // written, so every bot dirty at the start of a pass is written before any bot is written twice.
            ++stats.rounds;
            if (flushQueue.empty())
                for (auto const& [guid, entry] : entries)
                    if (entry.state == EntryState::Ready && entry.memory->IsDirty())
                        flushQueue.emplace_back(guid, entry.epoch);
        }
    }

    CoaMemoryRepository& repository = CoaMemoryRepository::Instance();
    for (uint32 n = 0; n < cfg.flushBotsPerTick && !flushQueue.empty(); ++n)
    {
        auto const [guid, epoch] = flushQueue.front();
        flushQueue.pop_front();
        auto const found = entries.find(guid);
        if (found == entries.end() || found->second.epoch != epoch || found->second.state != EntryState::Ready ||
            !found->second.memory->IsDirty())
            continue;

        BotMemory& memory = *found->second.memory;
        std::size_t const relations = memory.dirty.size();
        std::size_t const events = memory.unsaved.size();
        bool const trimDue = memory.eventsSinceTrim + events > 0;
        if (!repository.Flush(memory, cfg.maxEventsPerBot, false))
            continue;
        ++stats.flushedBots;
        stats.flushedRelations += relations;
        stats.flushedEvents += events;
        if (trimDue && !memory.eventsSinceTrim)
            ++stats.trims;
        stats.lastFlushMs = nowMs;
    }
}

void CoaMemoryMgr::Sweep()
{
    std::vector<uint32> gone;
    for (auto& [guid, entry] : entries)
    {
        if (LiveAi(guid, entry.epoch))
        {
            entry.missingSweeps = 0;
            continue;
        }
        if (++entry.missingSweeps >= SWEEPS_BEFORE_CLOSE)
            gone.push_back(guid);
    }

    // A BotLogout lost to a full bus: the bot of that life is gone, its memory is closed (or its load dropped).
    for (uint32 const guid : gone)
    {
        auto const found = entries.find(guid);
        if (found == entries.end())
            continue;
        LOG_DEBUG("playerbots.memory", "CoA memory: bot {} is gone without a logout, closing its memory", guid);
        if (found->second.state == EntryState::Ready)
            Close(guid);
        else
        {
            entries.erase(found);
            ++stats.loadsDropped;
        }
    }
}

void CoaMemoryMgr::Retain(uint32 now)
{
    CoaMemoryConfig const& cfg = Config();
    CoaMemoryRepository::Instance().Retention(now, cfg.eventRetentionDays, cfg.relationRetentionDays,
                                              cfg.retentionBatch);
    ++stats.retentionRounds;

    uint32 const eventCutoff = now > cfg.eventRetentionDays * 86400 ? now - cfg.eventRetentionDays * 86400 : 0;
    uint32 const relationCutoff = now > cfg.relationRetentionDays * 86400 ? now - cfg.relationRetentionDays * 86400 : 0;
    auto const trim = [eventCutoff, relationCutoff](BotMemory& memory)
    {
        std::size_t removed =
            std::erase_if(memory.events, [eventCutoff](MemoryEvent const& event) { return event.ts < eventCutoff; });
        for (auto it = memory.relations.begin(); it != memory.relations.end();)
        {
            if (it->second.lastSeen < relationCutoff)
            {
                memory.dirty.erase(it->first);
                it = memory.relations.erase(it);
                ++removed;
            }
            else
                ++it;
        }
        return removed;
    };
    for (auto& [guid, entry] : entries)
        if (entry.state == EntryState::Ready && trim(*entry.memory))
            entry.viewDirty = true;
    for (auto& [guid, closed] : recentlyClosed)
        trim(*closed.memory);
}

// --- Readers ---

MemoryTone CoaMemoryMgr::ToneFor(Player* bot, Player* other) const
{
    if (!CoaMemoryConfig::BehaviourActive(MemoryBehaviour::MemoryReplies))
        return MemoryTone::None;
    COA_ASSERT_WORLD_THREAD();
    if (!running || !bot || !other)
        return MemoryTone::None;

    auto const found = entries.find(bot->GetGUID().GetCounter());
    if (found == entries.end() || found->second.state != EntryState::Ready)
        return MemoryTone::None;
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);
    if (!ai || !ai->HasStrategy("coa memory", BOT_STATE_NON_COMBAT))
        return MemoryTone::None;
    MemoryRelation const* const relation = found->second.memory->Find(other->GetGUID().GetCounter());
    if (!relation)
        return MemoryTone::None;

    uint32 const account = other->GetSession() ? other->GetSession()->GetAccountId() : 0;
    switch (LabelOf(*relation, Config().decay, NowUnix()))
    {
        case MemoryLabel::Friend:
            return MemoryTone::Friend;
        case MemoryLabel::Disliked:
            return HostileOptedOut(account) ? MemoryTone::None : MemoryTone::Cold;
        case MemoryLabel::Grudge:
            return HostileOptedOut(account) ? MemoryTone::None : MemoryTone::Grudge;
        default:
            return MemoryTone::None;
    }
}

std::vector<std::string> CoaMemoryMgr::BrainLines(std::vector<ObjectGuid> const& bots, ObjectGuid speaker,
                                                  uint32 zoneId, uint32 maxBytes) const
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<std::string> lines;
    if (!running || !Config().stateTokenBudget || bots.empty() || !speaker)
        return lines;

    uint32 const now = NowUnix();
    uint32 const about = speaker.GetCounter();
    std::size_t used = 0;
    auto const add = [&](std::string line)
    {
        if (used + line.size() + 1 > maxBytes)
            return false;
        used += line.size() + 1;
        lines.push_back(std::move(line));
        return true;
    };
    auto const ready = [this](ObjectGuid bot) -> BotMemory const*
    {
        auto const found = entries.find(bot.GetCounter());
        return found != entries.end() && found->second.state == EntryState::Ready ? found->second.memory.get()
                                                                                  : nullptr;
    };

    for (std::size_t i = 0; i < bots.size(); ++i)
    {
        BotMemory const* const memory = ready(bots[i]);
        MemoryRelation const* const relation = memory ? memory->Find(about) : nullptr;
        if (relation &&
            !add(MemoryRetrieval::BrainRelationLine(Acore::StringFormat("b{}", i), *relation, Config().decay, now)))
            return lines;
    }

    // Only the events about the speaker are ranked: others would crowd them out of a short list.
    if (BotMemory const* const memory = ready(bots[0]))
        for (MemoryEvent const* event :
             MemoryRetrieval::Rank(*memory, about, zoneId, MemoryRetrieval::Context::Chat, now, 5, true))
            if (!add(MemoryRetrieval::BrainEventLine("b0", *event, now)))
                break;
    return lines;
}

std::shared_ptr<MemoryView const> CoaMemoryMgr::View(ObjectGuid bot) const
{
    COA_ASSERT_WORLD_THREAD();
    auto const found = entries.find(bot.GetCounter());
    if (found == entries.end() || found->second.state != EntryState::Ready)
        return nullptr;
    return found->second.view;
}

BotMemory const* CoaMemoryMgr::Memory(ObjectGuid bot) const
{
    COA_ASSERT_WORLD_THREAD();
    auto const found = entries.find(bot.GetCounter());
    if (found == entries.end() || found->second.state != EntryState::Ready)
        return nullptr;
    return found->second.memory.get();
}

std::string CoaMemoryMgr::FormatStats() const
{
    COA_ASSERT_WORLD_THREAD();
    std::shared_ptr<CoaMemoryConfig const> const current = CoaMemoryConfig::Current();
    uint64 const nowMs = CoaTime::NowMs();

    uint32 ready = 0;
    uint32 loading = 0;
    uint32 waiting = 0;
    uint64 relations = 0;
    uint64 events = 0;
    uint64 dirty = 0;
    uint64 unsaved = 0;
    for (auto const& [guid, entry] : entries)
    {
        if (entry.state != EntryState::Ready)
        {
            ++loading;
            continue;
        }
        ++ready;
        relations += entry.memory->relations.size();
        events += entry.memory->events.size();
        dirty += entry.memory->dirty.size();
        unsaved += entry.memory->unsaved.size();
        if (entry.viewDirty || entry.unpublished)
            ++waiting;
    }

    char const* const tablesText =
        tables == TablesState::Ok ? "ok" : (tables == TablesState::Missing ? "missing" : "unchecked");
    std::string const last =
        stats.lastFlushMs ? Acore::StringFormat("{} s ago", (nowMs - stats.lastFlushMs) / 1000) : std::string("never");
    CoaEventBus const& bus = CoaEventBus::Instance();

    std::ostringstream out;
    out << Acore::StringFormat(
               "CoA memory: enable={} running={} tables={} bots: ready={} loading={} kept={} "
               "relations={} events={} dirty={} unsaved={}",
               current->enable ? 1 : 0, running ? 1 : 0, tablesText, ready, loading, recentlyClosed.size(), relations,
               events, dirty, unsaved)
        << '\n'
        << Acore::StringFormat(
               "events: applied={} backlog={} dropped: no-memory={} loading-full={} prefs={} "
               "opted-out={} bot-to-bot={}",
               stats.applied, backlog.size(), stats.droppedNoMemory, stats.droppedLoadingFull, stats.droppedPrefs,
               stats.droppedOptedOut, stats.droppedBotToBot)
        << '\n'
        << Acore::StringFormat(
               "loads: started={} taken-back={} dropped={} notes: conversations={} brain-rated={} "
               "heuristic={}",
               stats.loads, stats.takenBack, stats.loadsDropped, stats.conversations, stats.brainRated, stats.heuristic)
        << '\n'
        << Acore::StringFormat(
               "flush: rounds={} skipped={} bots={} relations={} events={} trims={} final={} last={} "
               "queue={}",
               stats.rounds, stats.roundsSkipped, stats.flushedBots, stats.flushedRelations, stats.flushedEvents,
               stats.trims, stats.finalFlushes, last, PlayerbotsDatabase.QueueSize())
        << '\n'
        << Acore::StringFormat("views: built={} published={} waiting={} retention={} revenge-calls={}",
                               stats.viewsBuilt, stats.viewsPublished, waiting, stats.retentionRounds,
                               stats.revengeCalls)
        << '\n'
        << Acore::StringFormat("bus: posted={} dropped={} queued={}/{}", bus.Posted(), bus.Dropped(), bus.Pending(),
                               bus.Capacity());
    return out.str();
}
