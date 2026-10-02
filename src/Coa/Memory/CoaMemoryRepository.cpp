/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaMemoryRepository.h"
#include "BotMemory.h"
#include "CoaThread.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "MySQLConnection.h"
#include "QueryResult.h"
#include "StringFormat.h"
#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
// A missing table makes MySQLConnection abort the server (ER_NO_SUCH_TABLE), even from an async statement.
constexpr char const* TABLES_SQL =
    "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN "
    "('playerbots_coa_memory_relation', 'playerbots_coa_memory_event')";

constexpr char const* RELATION_COLUMNS =
    "other_guid, other_account, other_is_bot, other_name, affinity, trust, respect, rivalry, gratitude, grudge, fear, "
    "annoyance, familiarity, camaraderie, meetings, chats, helped_me, helped_them, killed_me, killed_them, duels_won, "
    "duels_lost, groups_shared, trades, first_seen, last_seen, decayed_at";

constexpr std::size_t RELATIONS_PER_STATEMENT = 32;
constexpr std::size_t EVENTS_PER_STATEMENT = 64;
constexpr std::size_t IDS_PER_STATEMENT = 500;
constexpr uint32 TRIM_EVERY = 32;

// Synchronous: the playerbots database's synchronous connection.
bool TablesExist()
{
    QueryResult const result = PlayerbotsDatabase.Query(TABLES_SQL);
    return result && result->Fetch()[0].Get<uint64>() == 2;
}

std::string Escaped(std::string text)
{
    PlayerbotsDatabase.EscapeString(text);
    return text;
}

template <typename It>
std::string JoinIds(It first, It last)
{
    std::string ids;
    for (It it = first; it != last; ++it)
    {
        if (!ids.empty())
            ids += ", ";
        ids += std::to_string(*it);
    }
    return ids;
}

// Sends `sql + "(ids)" + suffix` for each chunk of IDS_PER_STATEMENT ids, through `send`.
template <typename Send>
void ForIdChunks(std::vector<uint32> const& ids, Send const& send)
{
    for (std::size_t start = 0; start < ids.size(); start += IDS_PER_STATEMENT)
    {
        std::size_t const end = std::min(ids.size(), start + IDS_PER_STATEMENT);
        send(JoinIds(ids.begin() + start, ids.begin() + end));
    }
}

// Set once by SweepOrphansSync (startup, before the world runs; atomics for a later runtime CreateRandomBots).
std::atomic<bool> sweepAttempted{false};
std::atomic<bool> sweepDone{false};

// Synchronous. Whether `<chars>`.characters can be read through the playerbots connection: on another MySQL server
// a cross-schema statement fails with ER_NO_SUCH_TABLE, which aborts the server; without the privilege it fails.
// information_schema lists only the tables the user may read, and always exists.
bool CharactersReachable()
{
    std::string const chars = CharacterDatabase.GetConnectionInfo()->database;
    QueryResult const result = PlayerbotsDatabase.Query(Acore::StringFormat(
        "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = '{}' AND TABLE_NAME = 'characters'",
        Escaped(chars)));
    if (result && result->Fetch()[0].Get<uint64>() == 1)
        return true;
    LOG_WARN("playerbots.memory",
             "CoA memory: {}.characters is not reachable from the playerbots database; orphan sweep skipped", chars);
    return false;
}

std::vector<std::string> OrphanStatements()
{
    std::string const chars = CharacterDatabase.GetConnectionInfo()->database;
    return {Acore::StringFormat("DELETE r FROM playerbots_coa_memory_relation r LEFT JOIN `{}`.characters c "
                                "ON c.guid = r.bot_guid WHERE c.guid IS NULL",
                                chars),
            Acore::StringFormat("DELETE r FROM playerbots_coa_memory_relation r LEFT JOIN `{}`.characters c "
                                "ON c.guid = r.other_guid WHERE c.guid IS NULL",
                                chars),
            Acore::StringFormat("DELETE e FROM playerbots_coa_memory_event e LEFT JOIN `{}`.characters c "
                                "ON c.guid = e.bot_guid WHERE c.guid IS NULL",
                                chars),
            Acore::StringFormat("DELETE e FROM playerbots_coa_memory_event e LEFT JOIN `{}`.characters c "
                                "ON c.guid = e.other_guid WHERE e.other_guid <> 0 AND c.guid IS NULL",
                                chars)};
}

void AppendRelationValues(std::string& sql, uint32 botGuid, MemoryRelation const& relation)
{
    auto out = std::back_inserter(sql);
    Acore::StringFormatTo(out, "({}, {}, {}, {}, '{}'", botGuid, relation.otherGuid,
                          relation.otherIsBot ? 0u : relation.otherAccount, relation.otherIsBot ? 1u : 0u,
                          Escaped(relation.otherName));
    for (int16 const axis : relation.axes)
        Acore::StringFormatTo(out, ", {}", int32(axis));
    for (uint16 const counter : relation.counters)
        Acore::StringFormatTo(out, ", {}", uint32(counter));
    Acore::StringFormatTo(out, ", {}, {}, {})", relation.firstSeen, relation.lastSeen, relation.decayedAt);
}

void AppendEventValues(std::string& sql, uint32 botGuid, MemoryEvent const& event)
{
    std::string detail = event.detail;
    if (detail.size() > MEMORY_DETAIL_MAX)
        detail.resize(MEMORY_DETAIL_MAX);
    Acore::StringFormatTo(std::back_inserter(sql), "({}, {}, {}, {}, {}, {}, {}, {}, '{}')", botGuid, event.otherGuid,
                          event.otherAccount, uint32(event.kind), uint32(event.importance), uint32(event.mapId),
                          event.zoneId, event.ts, Escaped(std::move(detail)));
}
}  // namespace

CoaMemoryRepository& CoaMemoryRepository::Instance()
{
    static CoaMemoryRepository instance;
    return instance;
}

bool CoaMemoryRepository::CheckTables()
{
    COA_ASSERT_WORLD_THREAD();
    bool const ready = TablesExist();
    tablesReady.store(ready, std::memory_order_relaxed);
    if (!ready)
        LOG_ERROR("playerbots.memory",
                  "CoA memory: table playerbots_coa_memory_relation or playerbots_coa_memory_event is missing (apply "
                  "data/sql/playerbots/updates/2026_09_30_02_coa_memory.sql); memory stays off");
    return ready;
}

bool CoaMemoryRepository::SweepOrphansSync()
{
    if (sweepAttempted.exchange(true, std::memory_order_relaxed))
        return sweepDone.load(std::memory_order_relaxed);
    if (!TablesExist() || !CharactersReachable())
        return false;
    for (std::string const& sql : OrphanStatements())
        PlayerbotsDatabase.DirectExecute(sql);
    sweepDone.store(true, std::memory_order_relaxed);
    return true;
}

bool CoaMemoryRepository::StartupSwept() { return sweepAttempted.load(std::memory_order_relaxed); }

bool CoaMemoryRepository::DeleteOrphans()
{
    if (!TablesExist() || !CharactersReachable())
        return false;
    for (std::string const& sql : OrphanStatements())
        PlayerbotsDatabase.Execute(sql);
    return true;
}

QueryCallback CoaMemoryRepository::LoadRelations(uint32 botGuid, uint32 maxRelations)
{
    return PlayerbotsDatabase.AsyncQuery(
        Acore::StringFormat("SELECT {} FROM playerbots_coa_memory_relation WHERE bot_guid = {} ORDER BY last_seen "
                            "DESC LIMIT {}",
                            RELATION_COLUMNS, botGuid, maxRelations));
}

QueryCallback CoaMemoryRepository::LoadEvents(uint32 botGuid, uint32 maxEvents)
{
    return PlayerbotsDatabase.AsyncQuery(
        Acore::StringFormat("SELECT id, other_guid, other_account, kind, importance, map_id, zone_id, ts, detail FROM "
                            "playerbots_coa_memory_event WHERE bot_guid = {} ORDER BY ts DESC, id DESC LIMIT {}",
                            botGuid, maxEvents));
}

void CoaMemoryRepository::ReadRelations(QueryResult const& result, BotMemory& memory)
{
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        MemoryRelation relation;
        relation.otherGuid = fields[0].Get<uint32>();
        relation.otherAccount = fields[1].Get<uint32>();
        relation.otherIsBot = fields[2].Get<uint8>() != 0;
        relation.otherName = fields[3].Get<std::string>();
        for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
            relation.axes[i] = MemoryClampAxis(MemoryAxis(i), fields[4 + i].Get<int16>());
        for (std::size_t i = 0; i < MEMORY_COUNTER_COUNT; ++i)
            relation.counters[i] = fields[14 + i].Get<uint16>();
        relation.firstSeen = fields[24].Get<uint32>();
        relation.lastSeen = fields[25].Get<uint32>();
        relation.decayedAt = fields[26].Get<uint32>();
        relation.lastMetAt = relation.lastSeen;
        if (relation.otherIsBot)
            relation.otherAccount = 0;
        memory.relations[relation.otherGuid] = std::move(relation);
    } while (result->NextRow());
}

void CoaMemoryRepository::ReadEvents(QueryResult const& result, BotMemory& memory)
{
    if (!result)
        return;

    // Rows come newest first: the newest KilledBy of each other, and the DuelFled run before its newest won or
    // lost duel, are found on the way.
    std::vector<MemoryEvent> newestFirst;
    std::unordered_set<uint32> streakEnded;
    std::unordered_map<uint32, uint32> newerFlee;
    do
    {
        Field* fields = result->Fetch();
        uint8 const kind = fields[3].Get<uint8>();
        if (kind < uint8(MemoryEventKind::Met) || kind >= uint8(MemoryEventKind::Count))
            continue;

        MemoryEvent event;
        event.id = fields[0].Get<uint64>();
        event.otherGuid = fields[1].Get<uint32>();
        event.otherAccount = fields[2].Get<uint32>();
        event.kind = MemoryEventKind(kind);
        event.importance = std::min<uint8>(fields[4].Get<uint8>(), MEMORY_IMPORTANCE_MAX);
        event.mapId = fields[5].Get<uint16>();
        event.zoneId = fields[6].Get<uint32>();
        event.ts = fields[7].Get<uint32>();
        event.detail = fields[8].Get<std::string>();

        if (MemoryRelation* relation = memory.Find(event.otherGuid))
        {
            switch (event.kind)
            {
                case MemoryEventKind::KilledBy:
                    relation->lastKilledMeAt = std::max(relation->lastKilledMeAt, event.ts);
                    break;
                case MemoryEventKind::DuelFled:
                {
                    if (streakEnded.contains(event.otherGuid))
                        break;
                    // Same run as live: a gap of more than a day between two flees ends it.
                    auto const previous = newerFlee.find(event.otherGuid);
                    if (previous != newerFlee.end() &&
                        previous->second - event.ts > MemoryThreshold::DUEL_FLED_WINDOW)
                    {
                        streakEnded.insert(event.otherGuid);
                        break;
                    }
                    newerFlee[event.otherGuid] = event.ts;
                    relation->lastFledAt = std::max(relation->lastFledAt, event.ts);
                    if (relation->fledStreak < 255)
                        ++relation->fledStreak;
                    break;
                }
                case MemoryEventKind::DuelWon:
                case MemoryEventKind::DuelLost:
                    streakEnded.insert(event.otherGuid);
                    break;
                default:
                    break;
            }
        }
        newestFirst.push_back(std::move(event));
    } while (result->NextRow());

    for (auto it = newestFirst.rbegin(); it != newestFirst.rend(); ++it)
        memory.events.push_back(std::move(*it));
}

uint32 CoaMemoryRepository::Flush(BotMemory& memory, uint32 maxEventsPerBot, bool final)
{
    COA_ASSERT_WORLD_THREAD();
    if (!TablesReady())
        return 0;
    if (!memory.IsDirty() && !(final && memory.eventsSinceTrim))
        return 0;

    uint32 const bot = memory.BotGuid();
    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
    uint32 statements = 0;

    // 1. Evicted relations (a relation evicted then made again is dirty: its REPLACE below comes after).
    ForIdChunks(
        memory.deleted,
        [&](std::string const& ids)
        {
            trans->Append(Acore::StringFormat(
                "DELETE FROM playerbots_coa_memory_relation WHERE bot_guid = {} AND other_guid IN ({})", bot, ids));
            ++statements;
        });

    // 2. Dirty relations, absolute values.
    std::vector<MemoryRelation const*> dirty;
    dirty.reserve(memory.dirty.size());
    for (uint32 const otherGuid : memory.dirty)
        if (MemoryRelation const* relation = memory.Find(otherGuid))
            dirty.push_back(relation);
    for (std::size_t start = 0; start < dirty.size(); start += RELATIONS_PER_STATEMENT)
    {
        std::string sql =
            "REPLACE INTO playerbots_coa_memory_relation (bot_guid, other_guid, other_account, other_is_bot, "
            "other_name, affinity, trust, respect, rivalry, gratitude, grudge, fear, annoyance, familiarity, "
            "camaraderie, meetings, chats, helped_me, helped_them, killed_me, killed_them, duels_won, duels_lost, "
            "groups_shared, trades, first_seen, last_seen, decayed_at) VALUES ";
        std::size_t const end = std::min(dirty.size(), start + RELATIONS_PER_STATEMENT);
        for (std::size_t i = start; i < end; ++i)
        {
            if (i != start)
                sql += ", ";
            AppendRelationValues(sql, bot, *dirty[i]);
        }
        trans->Append(sql);
        ++statements;
    }

    // 3. Events made since the last flush.
    for (std::size_t start = 0; start < memory.unsaved.size(); start += EVENTS_PER_STATEMENT)
    {
        std::string sql =
            "INSERT INTO playerbots_coa_memory_event (bot_guid, other_guid, other_account, kind, "
            "importance, map_id, zone_id, ts, detail) VALUES ";
        std::size_t const end = std::min(memory.unsaved.size(), start + EVENTS_PER_STATEMENT);
        for (std::size_t i = start; i < end; ++i)
        {
            if (i != start)
                sql += ", ";
            AppendEventValues(sql, bot, memory.unsaved[i]);
        }
        trans->Append(sql);
        ++statements;
    }
    memory.eventsSinceTrim += uint32(memory.unsaved.size());

    // 4. Per-bot trim: rows past the newest MaxEventsPerBot, important ones kept. The derived table carries a LIMIT,
    // so MySQL materializes it (no error 1093); no such row leaves the comparison NULL and deletes nothing.
    if (memory.eventsSinceTrim >= TRIM_EVERY || (final && memory.eventsSinceTrim))
    {
        trans->Append(Acore::StringFormat(
            "DELETE FROM playerbots_coa_memory_event WHERE bot_guid = {} AND importance < 8 AND ts < (SELECT ts FROM "
            "(SELECT ts FROM playerbots_coa_memory_event WHERE bot_guid = {} ORDER BY ts DESC LIMIT {}, 1) AS cutoff) "
            "LIMIT 200",
            bot, bot, std::max<uint32>(maxEventsPerBot, 1) - 1));
        ++statements;
        memory.eventsSinceTrim = 0;
    }

    if (statements)
        PlayerbotsDatabase.CommitTransaction(trans);

    memory.deleted.clear();
    memory.dirty.clear();
    memory.unsaved.clear();
    return statements;
}

void CoaMemoryRepository::DeletePair(uint32 botGuid, uint32 otherGuid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!TablesReady())
        return;
    PlayerbotsDatabase.Execute(Acore::StringFormat(
        "DELETE FROM playerbots_coa_memory_relation WHERE bot_guid = {} AND other_guid = {}", botGuid, otherGuid));
    PlayerbotsDatabase.Execute(Acore::StringFormat(
        "DELETE FROM playerbots_coa_memory_event WHERE bot_guid = {} AND other_guid = {}", botGuid, otherGuid));
}

void CoaMemoryRepository::DeleteBot(uint32 botGuid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!TablesReady())
        return;
    PlayerbotsDatabase.Execute(
        Acore::StringFormat("DELETE FROM playerbots_coa_memory_relation WHERE bot_guid = {}", botGuid));
    PlayerbotsDatabase.Execute(
        Acore::StringFormat("DELETE FROM playerbots_coa_memory_event WHERE bot_guid = {}", botGuid));
}

void CoaMemoryRepository::PurgeCharacter(uint32 guid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!TablesReady() || !guid)
        return;
    PlayerbotsDatabase.Execute(
        Acore::StringFormat("DELETE FROM playerbots_coa_memory_relation WHERE bot_guid = {}", guid));
    PlayerbotsDatabase.Execute(
        Acore::StringFormat("DELETE FROM playerbots_coa_memory_relation WHERE other_guid = {}", guid));
    PlayerbotsDatabase.Execute(
        Acore::StringFormat("DELETE FROM playerbots_coa_memory_event WHERE bot_guid = {}", guid));
    PlayerbotsDatabase.Execute(
        Acore::StringFormat("DELETE FROM playerbots_coa_memory_event WHERE other_guid = {}", guid));
}

void CoaMemoryRepository::PurgeAccounts(std::vector<uint32> const& accounts)
{
    COA_ASSERT_WORLD_THREAD();
    if (!TablesReady())
        return;

    std::vector<uint32> ids;
    ids.reserve(accounts.size());
    for (uint32 const account : accounts)
        if (account && std::find(ids.begin(), ids.end(), account) == ids.end())
            ids.push_back(account);

    ForIdChunks(ids,
                [](std::string const& list)
                {
                    PlayerbotsDatabase.Execute(Acore::StringFormat(
                        "DELETE FROM playerbots_coa_memory_relation WHERE other_account IN ({})", list));
                    PlayerbotsDatabase.Execute(Acore::StringFormat(
                        "DELETE FROM playerbots_coa_memory_event WHERE other_account IN ({})", list));
                });
}

void CoaMemoryRepository::Retention(uint32 now, uint32 eventRetentionDays, uint32 relationRetentionDays, uint32 batch)
{
    if (!TablesReady())
        return;
    uint32 const eventCutoff = now > eventRetentionDays * 86400 ? now - eventRetentionDays * 86400 : 0;
    uint32 const relationCutoff = now > relationRetentionDays * 86400 ? now - relationRetentionDays * 86400 : 0;
    PlayerbotsDatabase.Execute(
        Acore::StringFormat("DELETE FROM playerbots_coa_memory_event WHERE ts < {} LIMIT {}", eventCutoff, batch));
    PlayerbotsDatabase.Execute(Acore::StringFormat(
        "DELETE FROM playerbots_coa_memory_relation WHERE last_seen < {} LIMIT {}", relationCutoff, batch));
}

QueryCallback CoaMemoryRepository::ShowRelations(uint32 botGuid, uint32 otherGuid, uint32 limit)
{
    std::string const other = otherGuid ? Acore::StringFormat(" AND other_guid = {}", otherGuid) : std::string();
    return PlayerbotsDatabase.AsyncQuery(
        Acore::StringFormat("SELECT {}, bot_guid FROM playerbots_coa_memory_relation WHERE bot_guid = {}{} ORDER BY "
                            "last_seen DESC LIMIT {}",
                            RELATION_COLUMNS, botGuid, other, limit));
}

QueryCallback CoaMemoryRepository::ShowEvents(uint32 botGuid, uint32 otherGuid, uint32 limit)
{
    std::string const other = otherGuid ? Acore::StringFormat(" AND other_guid = {}", otherGuid) : std::string();
    return PlayerbotsDatabase.AsyncQuery(
        Acore::StringFormat("SELECT other_guid, kind, importance, map_id, zone_id, ts, detail FROM "
                            "playerbots_coa_memory_event WHERE bot_guid = {}{} ORDER BY ts DESC, id DESC LIMIT {}",
                            botGuid, other, limit));
}

QueryCallback CoaMemoryRepository::MineRelations(uint32 accountId, uint32 offset, uint32 limit)
{
    return PlayerbotsDatabase.AsyncQuery(Acore::StringFormat(
        "SELECT bot_guid, other_guid, affinity, trust, respect, rivalry, gratitude, grudge, fear, annoyance, "
        "familiarity, camaraderie, meetings, chats, helped_me, helped_them, killed_me, killed_them, duels_won, "
        "duels_lost, groups_shared, trades, last_seen, decayed_at FROM playerbots_coa_memory_relation WHERE "
        "other_account = {} ORDER BY last_seen DESC LIMIT {} OFFSET {}",
        accountId, limit, offset));
}

QueryCallback CoaMemoryRepository::MineEvents(uint32 accountId, std::vector<uint32> const& botGuids, uint32 limit)
{
    // An empty list still makes a valid statement, which returns nothing.
    std::string const bots = botGuids.empty() ? std::string("0") : JoinIds(botGuids.begin(), botGuids.end());
    return PlayerbotsDatabase.AsyncQuery(
        Acore::StringFormat("SELECT bot_guid, other_guid, kind, ts FROM playerbots_coa_memory_event WHERE "
                            "other_account = {} AND bot_guid IN ({}) ORDER BY ts DESC LIMIT {}",
                            accountId, bots, limit));
}

QueryCallback CoaMemoryRepository::Counts()
{
    return PlayerbotsDatabase.AsyncQuery(
        "SELECT (SELECT COUNT(*) FROM playerbots_coa_memory_relation), (SELECT "
        "COUNT(*) FROM playerbots_coa_memory_event)");
}
