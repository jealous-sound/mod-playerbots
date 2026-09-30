/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYMGR_H
#define PLAYERBOTS_COAMEMORYMGR_H

#include "CoaEventBus.h"
#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "MemoryEvent.h"
#include "MemoryTypes.h"
#include "ObjectGuid.h"
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class BotMemory;
class MemoryView;
class Player;
class QueryCallback;
struct CoaMemoryConfig;
struct MemoryOther;
struct MemoryRelation;

// Prints an answer that arrives ticks after a command to the account that asked (0: the console). The same type
// as BrainAdminPrint, so CoaWorldScript passes one printer to both.
using CoaMemoryPrint = std::function<void(uint32 requesterAccount, std::string const& line)>;

// The bots' memory (M3). The one owner and writer of every BotMemory: a shared_ptr per (bot guid, epoch), on the
// world thread only, driven from CoaWorldScript::OnUpdate after the map threads are joined. Hooks post plain
// events to the CoaEventBus; the voice director calls OnConversation directly (it runs on the world thread too).
// The bot's own map thread reads a MemoryView snapshot published into its PlayerbotAI, never a BotMemory. A bot is
// deleted on a map thread: ~PlayerbotAI only posts BotLogout, so nothing here ever points into a PlayerbotAI
// across ticks (every tick looks the bot up again and checks its epoch).
//
// Every public function below is world thread only (COA_ASSERT_WORLD_THREAD) unless it says otherwise. With
// AiPlayerbot.CoaMemory.Enable = 0 nothing here runs: CoaWorldScript never calls Update while Running() is false
// and no memory event is queued, and every other entry point returns at once.
class CoaMemoryMgr
{
public:
    static CoaMemoryMgr& Instance();

    // --- CoaWorldScript ---

    // OnStartup, after the preferences' LoadSync. Enable = 1: CheckTables (sync), the orphan sweep (sync, when
    // OrphanSweepOnStartup and CreateRandomBots did not run it), then running. Tables missing: memory stays off (one
    // LOG_ERROR).
    void OnStartup();
    // OnAfterConfigLoad(reload = true), after CoaMemoryConfig::Publish. Off -> on: CheckTables (sync), running,
    // and every online eligible bot gets a login (spread over the next ticks). On -> off: stopped here (final flush
    // of everything, views cleared on the online bots, state dropped).
    void OnConfigReload();
    // Memory is on and its tables were found: true from the start until memory is turned off (stopped at once by
    // the reload) or shut down. Any thread: an atomic the world thread sets. CoaWorldScript updates the manager only
    // while it is true or memory events are left.
    static bool Running();
    // Every world tick while Running() or memory events were drained. Consumes `memoryEvents` (cleared), then:
    // callbacks, backlog (at most DrainPerTick events), publication, flush round, liveness sweep, retention.
    void Update(uint32 diff, std::vector<CoaEvent>& memoryEvents, CoaMemoryPrint const& print);
    // OnShutdown, after the bus is closed and the director shut down: applies `memoryEvents` (the last drain), then
    // the final flush of every memory (never skipped; PlayerbotsDatabase.Close drains it in OnDatabasesClosing).
    // Nothing runs afterwards; a late BotLogout from KickAll finds the bus closed.
    void Shutdown(std::vector<CoaEvent>& memoryEvents);

    // Moves the memory events of `events` to the end of `memoryEvents`, keeping the order of both (a stable split).
    // Any thread; pure.
    static void TakeMemoryEvents(std::vector<CoaEvent>& events, std::vector<CoaEvent>& memoryEvents);

    // --- Voice director ---

    // A real player's message or text emote addressed to one bot. Applied now when the bot's memory is ready,
    // queued while it loads, ignored when the bot keeps no memory, the speaker opted out of memory, or the
    // preferences are not loaded yet.
    void OnConversation(MemoryConversation const& note);
    // How `bot` should sound toward `other` (CoaMemory.MemoryReplies): None unless the switch is on, the bot has the
    // "coa memory" strategy and a ready memory holding a notable relation; Cold and Grudge fall back to None when
    // `other`'s account opted out of hostile bots (COA_OPTOUT_HOSTILE).
    MemoryTone ToneFor(Player* bot, Player* other) const;
    // The [memory] lines of a brain state, features only: for each of `bots` (ballot order, "b<i>") with a relation
    // to `speaker`, MemoryRetrieval::BrainRelationLine; then for bots[0] its best events about the speaker
    // (MemoryRetrieval::Rank, Context::Chat, `zoneId`), BrainEventLine each, at most 5. Stops before `maxBytes`.
    // Empty when memory is off, CoaMemory.StateTokenBudget is 0, or nothing is known.
    std::vector<std::string> BrainLines(std::vector<ObjectGuid> const& bots, ObjectGuid speaker, uint32 zoneId,
                                        uint32 maxBytes) const;

    // --- Readers ---

    // The newest view built for `bot` (possibly newer than the one its PlayerbotAI holds until the next
    // publication); null when it keeps no ready memory.
    std::shared_ptr<MemoryView const> View(ObjectGuid bot) const;
    // A ready memory; null otherwise. Do not keep the pointer past the current call.
    BotMemory const* Memory(ObjectGuid bot) const;

    // --- Commands (through CoaMemoryAdmin and the optout command) ---

    // RAM and database: every relation and event about characters of `accountId` (never 0), in every bot. Rebuilds
    // the views concerned. No-op (false) when memory is not running.
    bool PurgeAccount(uint32 accountId);

    // .playerbots memory stats, several lines separated by '\n'.
    std::string FormatStats() const;

private:
    CoaMemoryMgr() = default;
    CoaMemoryMgr(CoaMemoryMgr const&) = delete;
    CoaMemoryMgr& operator=(CoaMemoryMgr const&) = delete;

    friend class CoaMemoryAdmin;

    enum class EntryState : uint8
    {
        Loading = 0,  // the two async queries are in flight; inputs wait in `pending*`
        Ready
    };

    struct Entry
    {
        uint64 epoch = 0;
        EntryState state = EntryState::Loading;
        std::shared_ptr<BotMemory> memory;
        std::shared_ptr<MemoryView const> view;  // the newest built
        bool viewDirty = false;                  // build and publish at the next chance
        uint8 missingSweeps = 0;                 // liveness sweeps in a row that did not find the bot's life
        std::vector<CoaEvent> pendingEvents;           // deeds that arrived while loading, at most 64
        std::vector<MemoryConversation> pendingNotes;  // conversations that arrived while loading, at most 64
        bool unpublished = false;  // `view` built but not handed to the bot yet (not found in world): retried
        uint32 loadId = 0;         // the load in flight; a command that changed the rows meanwhile starts another
        // Other low guid -> unix time the pair's current group began (GroupJoin; RAM only, only this group's
        // members). A kick later than KICK_DISMISSAL_SECONDS after it is a dismissal, not a hostile kick.
        std::unordered_map<uint32, uint32> groupedSince;
    };

    // A memory whose bot logged out, kept one flush interval so a quick relog takes it back without a query.
    struct Closed
    {
        std::shared_ptr<BotMemory> memory;
        uint64 closedMs = 0;
    };

    // The store adds the rest (flush round, timers, counters) as private members. The QueryCallbackProcessor of the
    // loads and command queries is a file-static of CoaMemoryMgr.cpp, like CoaPlayerPrefs': world thread only.
    std::unordered_map<uint32, Entry> entries;   // bot low guid -> its current life
    std::unordered_map<uint32, Closed> recentlyClosed;
    std::vector<CoaEvent> backlog;               // drained memory events not applied yet (DrainPerTick)
    CoaMemoryPrint printer;                      // the last printer Update was given (async command answers)
    bool running = false;
    bool stopped = false;                        // Shutdown ran

    // The store's privates (CoaMemoryMgr.cpp). World thread only.
    enum class TablesState : uint8
    {
        Unchecked = 0,
        Ok,
        Missing
    };

    struct Stats
    {
        uint64 applied = 0;
        uint64 droppedNoMemory = 0;
        uint64 droppedLoadingFull = 0;
        uint64 droppedPrefs = 0;
        uint64 droppedOptedOut = 0;
        uint64 droppedBotToBot = 0;
        uint64 conversations = 0;
        uint64 brainRated = 0;
        uint64 heuristic = 0;
        uint64 rounds = 0;
        uint64 roundsSkipped = 0;
        uint64 flushedBots = 0;
        uint64 flushedRelations = 0;
        uint64 flushedEvents = 0;
        uint64 trims = 0;
        uint64 finalFlushes = 0;
        uint64 lastFlushMs = 0;
        uint64 viewsBuilt = 0;
        uint64 viewsPublished = 0;
        uint64 retentionRounds = 0;
        uint64 revengeCalls = 0;
        uint64 loads = 0;
        uint64 takenBack = 0;
        uint64 loadsDropped = 0;
    };

    std::shared_ptr<CoaMemoryConfig const> config;       // refreshed by every world-thread entry point
    std::deque<std::pair<uint32, uint64>> flushQueue;    // (bot guid, epoch) of the current flush round
    std::deque<std::pair<uint32, uint64>> pendingOnlineLogins;  // bots online when memory was turned on
    uint64 nextRoundMs = 0;
    uint64 nextSweepMs = 0;
    uint64 nextRetentionMs = 0;
    uint32 nextLoadId = 0;
    bool optOutPurgeDone = false;
    bool shuttingDown = false;                           // Shutdown applies the last events: no new login
    bool ownerSeen = false;                              // Apply: some character of the event keeps a memory
    TablesState tables = TablesState::Unchecked;
    Stats stats;

    CoaMemoryConfig const& Config() const;
    void SetRunning(bool on);  // `running` and the atomic behind Running()
    void Start(char const* why);
    void Stop();
    void Login(uint32 guid, uint64 epoch);
    void Logout(uint32 guid, uint64 epoch);
    void StartLoad(uint32 guid, Entry& entry);
    void FinishLoad(uint32 guid, uint64 epoch, uint32 loadId, QueryResult const& relations,
                    QueryResult const& events);
    void RestartLoads(uint32 onlyGuid);  // 0: every entry still loading
    void Close(uint32 guid);
    void FinalFlush(BotMemory& memory);
    void AddQuery(QueryCallback&& query);

    // Inputs (section 4 of the contract). `onlyOwner` != 0: replay of a loading bot's queue, its side only.
    void Apply(CoaEvent const& event, uint32 onlyOwner);
    void ApplyNote(MemoryConversation const& note);
    Entry* OwnerEntry(uint32 owner, CoaEvent const& event, uint32 onlyOwner);
    // A bot for memory is a character of a random bot account (random and addclass bots); altbots and self-bots on a
    // real account are that account's characters (their account is stored, so opt-out, purge and mine cover them).
    bool Identify(ObjectGuid guid, MemoryOther& other) const;
    bool OtherAllowed(MemoryOther const& other);
    MemoryRelation& Meet(BotMemory& memory, uint32 otherGuid, MemoryOther const& other, uint32 now, uint16 mapId,
                         uint32 zoneId);
    void Diary(BotMemory& memory, MemoryRelation const& relation, MemoryEventKind kind, uint16 mapId, uint32 zoneId,
               uint32 now, std::string detail);
    void ApplyDuel(CoaEvent const& event, uint32 onlyOwner, uint32 now);
    void ApplyPlayerKill(CoaEvent const& event, uint32 onlyOwner, uint32 now);
    void ApplyBossKill(CoaEvent const& event, uint32 onlyOwner, uint32 now);
    void ApplyCreatureHelp(CoaEvent const& event, uint32 onlyOwner, uint32 now);
    void ApplyGroupJoin(CoaEvent const& event, uint32 onlyOwner, uint32 now);
    void ApplyGroupLeave(CoaEvent const& event, uint32 onlyOwner, uint32 now);
    void ApplyTrade(CoaEvent const& event, uint32 onlyOwner, uint32 now);
    void RevengeCall(uint32 owner, Entry& entry, MemoryRelation& relation, MemoryOther const& killer, uint32 zoneId,
                     uint32 now);

    // Forgetting (commands, opt-outs, deleted characters, retention). RAM only; the caller sends the statements.
    void ForgetAccounts(std::unordered_set<uint32> const& accounts);
    void ForgetCharacter(uint32 guid);

    void Publish(uint32 now);
    std::shared_ptr<MemoryView const> BuildView(uint32 guid, Entry const& entry, uint32 now) const;
    bool Deliver(uint32 guid, uint64 epoch, std::shared_ptr<MemoryView const> view);
    void FlushRound(uint64 nowMs);
    void Sweep();
    void Retain(uint32 now);
};

#endif
