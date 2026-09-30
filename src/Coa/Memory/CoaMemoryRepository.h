/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYREPOSITORY_H
#define PLAYERBOTS_COAMEMORYREPOSITORY_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "QueryCallback.h"
#include <atomic>
#include <string>
#include <vector>

class BotMemory;

// All SQL of the bots' memory: playerbots_coa_memory_relation and playerbots_coa_memory_event in the playerbots
// database (data/sql/playerbots/updates/2026_09_30_02_coa_memory.sql). Ad-hoc statements built with
// Acore::StringFormat and sent whole (never a player's text as a format string); every text value goes through
// PlayerbotsDatabase.EscapeString. The statements are exactly those of the contract.
//
// Threads: the world thread only (EscapeString uses the synchronous connection without a lock), except
// SweepOrphansSync and DeleteOrphans, which RandomPlayerbotFactory calls at startup before the world runs. Nothing
// here touches a table before CheckTables (or the static functions' own check) saw both: a query on a missing table
// aborts the server. The orphan deletes read the characters schema through the playerbots connection: they are sent
// only when information_schema shows that table there (another MySQL server would abort the server too).
class CoaMemoryRepository
{
public:
    static CoaMemoryRepository& Instance();

    // Synchronous information_schema check of both tables. Logs one LOG_ERROR naming the update file when one is
    // missing. Sets TablesReady.
    bool CheckTables();
    bool TablesReady() const { return tablesReady.load(std::memory_order_relaxed); }

    // Rows whose bot_guid or other_guid is no longer a character, synchronous (DirectExecute), at most once per
    // process: RandomPlayerbotFactory::CreateRandomBots before it creates a character (a new bot may get a deleted
    // character's guid), else CoaMemoryMgr::OnStartup, both with Enable = 1 and OrphanSweepOnStartup = 1. Its own
    // table check (missing: nothing, CheckTables logs it); characters schema not reachable: one LOG_WARN, nothing
    // sent. True when the statements were sent; a later call returns the first call's answer.
    static bool SweepOrphansSync();
    // SweepOrphansSync was called in this process, whatever it found.
    static bool StartupSwept();

    // The bot wipe (RandomPlayerbotFactory::CreateRandomBots with DeleteRandomBotAccounts = 1), once the characters
    // are deleted: the same orphan deletes, sent with async Execute (the wipe then waits for the queue). Runs its
    // own synchronous checks, whatever CoaMemory.Enable says; false (and no statement) when a table is missing or
    // the characters schema is not reachable.
    static bool DeleteOrphans();

    // The load of one bot (async; the caller chains them and adds the chain to its QueryCallbackProcessor).
    QueryCallback LoadRelations(uint32 botGuid, uint32 maxRelations);
    QueryCallback LoadEvents(uint32 botGuid, uint32 maxEvents);
    // Fill `memory` from those results (null: no rows). Relations get lastMetAt = last_seen; events are appended
    // oldest first; lastKilledMeAt and fledStreak are rebuilt from the events. Nothing is marked dirty.
    static void ReadRelations(QueryResult const& result, BotMemory& memory);
    static void ReadEvents(QueryResult const& result, BotMemory& memory);

    // One transaction (CommitTransaction, async) with, in order: DELETE of evicted relations, REPLACE of dirty
    // relations (32 rows per statement), INSERT of unsaved events (64 rows per statement), and the per-bot trim when
    // eventsSinceTrim reached 32 (or `final` and it is not 0). Clears dirty, unsaved and deleted. Returns the number
    // of statements (0: nothing was sent).
    uint32 Flush(BotMemory& memory, uint32 maxEventsPerBot, bool final);

    // Async Execute, integers only.
    void DeletePair(uint32 botGuid, uint32 otherGuid);
    void DeleteBot(uint32 botGuid);
    void PurgeCharacter(uint32 guid);  // as bot and as other
    void PurgeAccounts(std::vector<uint32> const& accounts);  // other_account IN (...), 500 ids per statement
    void Retention(uint32 now, uint32 eventRetentionDays, uint32 relationRetentionDays, uint32 batch);

    // GM and player commands (async).
    QueryCallback ShowRelations(uint32 botGuid, uint32 otherGuid, uint32 limit);  // otherGuid 0: all
    QueryCallback ShowEvents(uint32 botGuid, uint32 otherGuid, uint32 limit);
    QueryCallback MineRelations(uint32 accountId, uint32 offset, uint32 limit);
    QueryCallback MineEvents(uint32 accountId, std::vector<uint32> const& botGuids, uint32 limit);
    QueryCallback Counts();

private:
    CoaMemoryRepository() = default;
    CoaMemoryRepository(CoaMemoryRepository const&) = delete;
    CoaMemoryRepository& operator=(CoaMemoryRepository const&) = delete;

    std::atomic<bool> tablesReady{false};
};

#endif
