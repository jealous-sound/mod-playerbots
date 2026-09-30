/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaPlayerPrefs.h"
#include "AsyncCallbackProcessor.h"
#include "CoaThread.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryCallback.h"
#include "QueryResult.h"
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
// Touched on the world thread only (LoadAsync, ProcessCallbacks).
QueryCallbackProcessor& LoadCallbacks()
{
    static QueryCallbackProcessor processor;
    return processor;
}

// A missing table makes MySQLConnection abort the server (ER_NO_SUCH_TABLE), even from an async query, so both
// loads ask information_schema first and the writes wait for that answer.
constexpr char const* TABLE_CHECK_SQL =
    "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
    "AND TABLE_NAME = 'playerbots_coa_player_prefs'";
constexpr char const* LOAD_SQL =
    "SELECT account_id, ai_text, voice, memory, activity_mask, disclosed_at, changed_at "
    "FROM playerbots_coa_player_prefs";

void WriteRow(uint32 accountId, CoaPrefs const& row)
{
    PlayerbotsDatabase.Execute(
        "REPLACE INTO playerbots_coa_player_prefs (account_id, ai_text, voice, memory, activity_mask, disclosed_at, "
        "changed_at) VALUES ({}, {}, {}, {}, {}, {}, {})",
        accountId, uint32(row.aiText), uint32(row.voice), uint32(row.memory), row.activityMask, row.disclosedAt,
        row.changedAt);
}

void ApplyChange(CoaPrefs& row, CoaPrefTopic topic, bool optOut, uint32 changedAt)
{
    uint8 const value = optOut ? COA_PREF_OUT : COA_PREF_IN;
    auto const setBits = [&](uint32 bits)
    {
        if (optOut)
            row.activityMask |= bits;
        else
            row.activityMask &= ~bits;
    };

    switch (topic)
    {
        case CoaPrefTopic::Ai:
            row.aiText = value;
            break;
        case CoaPrefTopic::Voice:
            row.voice = value;
            break;
        case CoaPrefTopic::Memory:
            row.memory = value;
            break;
        case CoaPrefTopic::Duels:
            setBits(COA_OPTOUT_DUELS);
            break;
        case CoaPrefTopic::Hostile:
            setBits(COA_OPTOUT_HOSTILE);
            break;
        case CoaPrefTopic::All:
            row.aiText = value;
            row.voice = value;
            row.memory = value;
            setBits(COA_OPTOUT_DUELS | COA_OPTOUT_HOSTILE);
            break;
    }

    row.changedAt = changedAt;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        char x = a[i];
        if (x >= 'A' && x <= 'Z')
            x = char(x - 'A' + 'a');
        if (x != b[i])
            return false;
    }
    return true;
}
}  // namespace

CoaPlayerPrefs& CoaPlayerPrefs::Instance()
{
    static CoaPlayerPrefs instance;
    return instance;
}

void CoaPlayerPrefs::LoadSync()
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped || loading || IsLoaded())
        return;

    // The synchronous connection: the async worker may be busy for minutes (item caches at a first start).
    if (!PlayerbotsDatabase.Query(TABLE_CHECK_SQL))
    {
        OnTableMissing();
        return;
    }
    QueryResult const result = PlayerbotsDatabase.Query(LOAD_SQL);
    OnLoaded(result.get());
}

void CoaPlayerPrefs::LoadAsync()
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped || loading || IsLoaded())
        return;

    loading = true;
    // Both callbacks run on the world thread, from ProcessCallbacks.
    LoadCallbacks().AddCallback(
        PlayerbotsDatabase.AsyncQuery(TABLE_CHECK_SQL)
            .WithChainingCallback(
                [this](QueryCallback& callback, QueryResult exists)
                {
                    if (stopped)
                        return;
                    if (exists)
                    {
                        callback.SetNextQuery(PlayerbotsDatabase.AsyncQuery(LOAD_SQL));
                        return;
                    }
                    // Ending the chain here: the load callback below does not run.
                    OnTableMissing();
                })
            .WithCallback(
                [this](QueryResult result)
                {
                    if (!stopped)
                        OnLoaded(result.get());
                }));
}

void CoaPlayerPrefs::OnTableMissing()
{
    LOG_ERROR("playerbots",
              "CoA: table playerbots_coa_player_prefs is missing; apply "
              "data/sql/playerbots/updates/2026_09_30_00_coa_player_prefs.sql. Opt-outs are kept in memory only until "
              "then.");
    tableMissing = true;
    changedBeforeLoad.clear();
    changesBeforeLoad.clear();
    loading = false;
    loaded.store(true, std::memory_order_release);
}

void CoaPlayerPrefs::OnLoaded(ResultSet* result)
{
    uint32 count = 0;
    std::vector<std::pair<uint32, CoaPrefs>> changed;
    {
        std::unique_lock<std::shared_mutex> guard(lock);
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                CoaPrefs& row = prefs[fields[0].Get<uint32>()];
                row.aiText = fields[1].Get<uint8>();
                row.voice = fields[2].Get<uint8>();
                row.memory = fields[3].Get<uint8>();
                row.activityMask = fields[4].Get<uint32>();
                row.disclosedAt = fields[5].Get<uint32>();
                row.changedAt = fields[6].Get<uint32>();
                ++count;
            } while (result->NextRow());
        }

        // Changes made in game while the query ran are newer: replayed over the stored rows.
        for (PendingChange const& change : changesBeforeLoad)
            ApplyChange(prefs[change.accountId], change.topic, change.optOut, change.changedAt);

        for (uint32 const accountId : changedBeforeLoad)
            changed.emplace_back(accountId, prefs[accountId]);
    }

    changedBeforeLoad.clear();
    changesBeforeLoad.clear();
    loading = false;
    loaded.store(true, std::memory_order_release);
    LOG_INFO("playerbots", ">> Loaded {} CoA player preferences", count);

    // Written only now: the table is known to exist.
    for (auto const& [accountId, row] : changed)
        WriteRow(accountId, row);
}

void CoaPlayerPrefs::ProcessCallbacks()
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped)
        return;
    LoadCallbacks().ProcessReadyCallbacks();
}

CoaPrefs CoaPlayerPrefs::Get(uint32 accountId) const
{
    std::shared_lock<std::shared_mutex> guard(lock);
    auto const found = prefs.find(accountId);
    return found != prefs.end() ? found->second : CoaPrefs();
}

bool CoaPlayerPrefs::VoiceOptedOut(uint32 accountId) const
{
    std::shared_lock<std::shared_mutex> guard(lock);
    auto const found = prefs.find(accountId);
    return found != prefs.end() && found->second.VoiceOptedOut();
}

std::vector<uint32> CoaPlayerPrefs::MemoryOptOuts() const
{
    std::vector<uint32> accounts;
    std::shared_lock<std::shared_mutex> guard(lock);
    for (auto const& [accountId, row] : prefs)
        if (row.MemoryOptedOut())
            accounts.push_back(accountId);
    return accounts;
}

void CoaPlayerPrefs::SetOptOut(uint32 accountId, CoaPrefTopic topic, bool optOut)
{
    COA_ASSERT_WORLD_THREAD();
    if (stopped || !accountId)
        return;

    uint32 const changedAt = uint32(time(nullptr));
    CoaPrefs row;
    {
        std::unique_lock<std::shared_mutex> guard(lock);
        CoaPrefs& cached = prefs[accountId];
        ApplyChange(cached, topic, optOut, changedAt);
        row = cached;
    }

    if (tableMissing)
        return;

    // Before the load returned: the cache answers at once, and the change waits to be replayed over the stored row
    // (so the other columns survive) and written by the load callback.
    if (!IsLoaded())
    {
        changedBeforeLoad.insert(accountId);
        changesBeforeLoad.push_back({accountId, topic, optOut, changedAt});
        LoadAsync();
        return;
    }

    WriteRow(accountId, row);
}

void CoaPlayerPrefs::Shutdown()
{
    COA_ASSERT_WORLD_THREAD();
    // A load still in flight is never processed: its future is dropped with the processor at exit.
    stopped = true;
    loading = false;
}

std::optional<CoaPrefTopic> CoaPlayerPrefs::ParseTopic(std::string_view word)
{
    static constexpr CoaPrefTopic TOPICS[] = {CoaPrefTopic::Ai,    CoaPrefTopic::Voice,   CoaPrefTopic::Memory,
                                              CoaPrefTopic::Duels, CoaPrefTopic::Hostile, CoaPrefTopic::All};
    for (CoaPrefTopic const topic : TOPICS)
        if (EqualsIgnoreCase(word, TopicName(topic)))
            return topic;
    return std::nullopt;
}

char const* CoaPlayerPrefs::TopicName(CoaPrefTopic topic)
{
    switch (topic)
    {
        case CoaPrefTopic::Ai:
            return "ai";
        case CoaPrefTopic::Voice:
            return "voice";
        case CoaPrefTopic::Memory:
            return "memory";
        case CoaPrefTopic::Duels:
            return "duels";
        case CoaPrefTopic::Hostile:
            return "hostile";
        case CoaPrefTopic::All:
            return "all";
    }
    return "all";
}
