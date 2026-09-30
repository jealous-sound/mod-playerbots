/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityStore.h"
#include "AsyncCallbackProcessor.h"
#include "CoaText.h"
#include "CoaThread.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryCallback.h"
#include "QueryResult.h"
#include "StringFormat.h"
#include <algorithm>
#include <unordered_set>

namespace
{
// A missing table makes MySQLConnection abort the server (ER_NO_SUCH_TABLE), even from an async statement.
constexpr char const* TABLES_SQL =
    "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN "
    "('playerbots_coa_activity', 'playerbots_coa_activity_member')";

// World thread only.
QueryCallbackProcessor& Callbacks()
{
    static QueryCallbackProcessor processor;
    return processor;
}

// Reset() bumps it: a callback of an older generation does nothing when it finally runs.
uint32 generation = 0;

std::string Escaped(std::string text, std::size_t maxBytes)
{
    CoaText::CutUtf8Bytes(text, maxBytes);
    PlayerbotsDatabase.EscapeString(text);
    return text;
}

uint32 Cutoff(uint32 nowUnix, uint32 days) { return nowUnix > days * 86400 ? nowUnix - days * 86400 : 0; }
}  // namespace

ActivityStore& ActivityStore::Instance()
{
    static ActivityStore instance;
    return instance;
}

bool ActivityStore::CheckTables()
{
    COA_ASSERT_WORLD_THREAD();
    QueryResult const result = PlayerbotsDatabase.Query(TABLES_SQL);
    tablesReady = result && result->Fetch()[0].Get<uint64>() == 2;
    return tablesReady;
}

uint32 ActivityStore::LoadMaxId()
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return 0;
    QueryResult const result = PlayerbotsDatabase.Query("SELECT COALESCE(MAX(id), 0) FROM playerbots_coa_activity");
    return result ? uint32(result->Fetch()[0].Get<uint64>()) : 0;
}

void ActivityStore::AbortLiveRows(uint32 nowUnix, uint32 logKeepDays)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    giveUpBeforeUnix = Cutoff(nowUnix, logKeepDays);

    // One transaction: the member rows are closed with the ended time the first statement wrote, whatever the
    // number of database workers.
    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
    trans->Append(Acore::StringFormat("UPDATE playerbots_coa_activity SET state = {}, ended = {}, outcome = {}, "
                                      "summary = 'server restart' WHERE state < {}",
                                      uint32(ACTIVITY_STATE_ABORTED), nowUnix, uint32(ActivityOutcome::Restart),
                                      uint32(ACTIVITY_STATE_DONE)));
    trans->Append(Acore::StringFormat("UPDATE playerbots_coa_activity_member m JOIN playerbots_coa_activity a ON a.id "
                                      "= m.activity_id SET m.left_at = a.ended WHERE m.left_at = 0 AND a.state >= {}",
                                      uint32(ACTIVITY_STATE_DONE)));
    trans->Append(Acore::StringFormat(
        "UPDATE playerbots_coa_activity_member SET restored = 1 WHERE restored = 0 AND joined < {}", giveUpBeforeUnix));
    PlayerbotsDatabase.CommitTransaction(trans);
}

std::vector<ActivityRestoreRow> ActivityStore::LoadUnrestored()
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<ActivityRestoreRow> rows;
    if (!tablesReady)
        return rows;

    // The synchronous connection does not wait for AbortLiveRows' async transaction: its give-up rule is applied
    // here too.
    QueryResult const result = PlayerbotsDatabase.Query(
        Acore::StringFormat("SELECT guid, prev_ruleset FROM playerbots_coa_activity_member WHERE restored = 0 AND "
                            "prev_ruleset <> 0 AND joined >= {} ORDER BY joined DESC",
                            giveUpBeforeUnix));
    if (!result)
        return rows;

    std::unordered_set<uint32> seen;
    do
    {
        Field const* fields = result->Fetch();
        ActivityRestoreRow row;
        row.guid = fields[0].Get<uint32>();
        row.prevRuleset = fields[1].Get<uint32>();
        if (seen.insert(row.guid).second)
            rows.push_back(row);
    } while (result->NextRow());
    return rows;
}

void ActivityStore::InsertActivity(uint32 id, ActivityKindId kind, uint8 team, std::string const& target,
                                   uint32 leaderGuid, ActivityPhase phase, uint32 startedUnix)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    PlayerbotsDatabase.Execute(Acore::StringFormat(
        "INSERT INTO playerbots_coa_activity (id, kind, team, target, leader_guid, state, started) VALUES ({}, '{}', "
        "{}, '{}', {}, {}, {})",
        id, ActivityKindName(kind), uint32(team), Escaped(target, 64), leaderGuid, uint32(phase), startedUnix));
}

void ActivityStore::UpdatePhase(uint32 id, ActivityPhase phase)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    PlayerbotsDatabase.Execute(
        Acore::StringFormat("UPDATE playerbots_coa_activity SET state = {} WHERE id = {}", uint32(phase), id));
}

void ActivityStore::UpdateLeader(uint32 id, uint32 leaderGuid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    PlayerbotsDatabase.Execute(
        Acore::StringFormat("UPDATE playerbots_coa_activity SET leader_guid = {} WHERE id = {}", leaderGuid, id));
}

void ActivityStore::EndActivity(uint32 id, ActivityOutcome outcome, uint32 endedUnix, uint32 bots, uint32 players,
                                std::string const& summary)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    uint32 const state = outcome == ActivityOutcome::Success ? ACTIVITY_STATE_DONE : ACTIVITY_STATE_ABORTED;
    PlayerbotsDatabase.Execute(Acore::StringFormat(
        "UPDATE playerbots_coa_activity SET state = {}, ended = {}, outcome = {}, bots = {}, players = {}, summary = "
        "'{}' WHERE id = {}",
        state, endedUnix, uint32(outcome), std::min<uint32>(bots, 0xFFFF), std::min<uint32>(players, 0xFFFF),
        Escaped(summary, 255), id));
}

void ActivityStore::InsertMember(uint32 activityId, uint32 guid, bool isBot, ActivityRole role, uint32 joinedUnix)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    // A real player who left and joined the same activity again reopens its row.
    PlayerbotsDatabase.Execute(Acore::StringFormat(
        "INSERT INTO playerbots_coa_activity_member (activity_id, guid, is_bot, role, joined) VALUES ({}, {}, {}, {}, "
        "{}) ON DUPLICATE KEY UPDATE left_at = 0",
        activityId, guid, uint32(isBot ? 1 : 0), uint32(role), joinedUnix));
}

void ActivityStore::MemberLeft(uint32 activityId, uint32 guid, uint32 leftUnix)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    PlayerbotsDatabase.Execute(Acore::StringFormat("UPDATE playerbots_coa_activity_member SET left_at = {} WHERE "
                                                   "activity_id = {} AND guid = {} AND left_at = 0",
                                                   leftUnix, activityId, guid));
}

void ActivityStore::SetPrevRuleset(uint32 activityId, uint32 guid, uint32 prevRuleset)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    PlayerbotsDatabase.Execute(Acore::StringFormat("UPDATE playerbots_coa_activity_member SET prev_ruleset = {}, "
                                                   "restored = 0 WHERE activity_id = {} AND guid = {}",
                                                   prevRuleset, activityId, guid));
}

void ActivityStore::MarkRestored(uint32 guid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    PlayerbotsDatabase.Execute(Acore::StringFormat(
        "UPDATE playerbots_coa_activity_member SET restored = 1 WHERE guid = {} AND restored = 0", guid));
}

void ActivityStore::Retention(uint32 nowUnix, uint32 logKeepDays, uint32 nextId)
{
    COA_ASSERT_WORLD_THREAD();
    if (!tablesReady)
        return;
    uint32 const gen = generation;
    Callbacks().AddCallback(
        PlayerbotsDatabase
            .AsyncQuery(Acore::StringFormat(
                "SELECT COALESCE(MIN(id), {}) FROM playerbots_coa_activity WHERE started >= {}", nextId,
                Cutoff(nowUnix, logKeepDays)))
            .WithCallback(
                [gen](QueryResult result)
                {
                    if (gen != generation || !result || !ActivityStore::Instance().TablesReady())
                        return;
                    uint32 const minKept = uint32(result->Fetch()[0].Get<uint64>());
                    if (!minKept)
                        return;
                    PlayerbotsDatabase.Execute(Acore::StringFormat("DELETE FROM playerbots_coa_activity_member WHERE "
                                                                   "activity_id < {} AND restored = 1 LIMIT 5000",
                                                                   minKept));
                    PlayerbotsDatabase.Execute(Acore::StringFormat(
                        "DELETE FROM playerbots_coa_activity WHERE id < {} AND state >= {} LIMIT 1000", minKept,
                        uint32(ACTIVITY_STATE_DONE)));
                }));
}

void ActivityStore::ProcessCallbacks()
{
    COA_ASSERT_WORLD_THREAD();
    Callbacks().ProcessReadyCallbacks();
}

void ActivityStore::Reset()
{
    COA_ASSERT_WORLD_THREAD();
    ++generation;
}
