/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainUsageStore.h"
#include "BrainGovernor.h"
#include "BrainTypes.h"
#include "CoaThread.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryResult.h"
#include <ctime>
#include <string>
#include <vector>

namespace
{
constexpr uint64 FLUSH_EVERY_MS = 60000;
constexpr int64 RETENTION_DAYS = 90;

// A missing table makes MySQLConnection abort the server (ER_NO_SUCH_TABLE), so the load asks
// information_schema first and nothing else touches the table when it is missing.
constexpr char const* TABLE_CHECK_SQL =
    "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() "
    "AND TABLE_NAME = 'playerbots_coa_brain_usage'";

// Engine names come from the conf ([a-z0-9_-]) or are the table's own rows; anything else is never written, so no
// quoting is needed in the statements below.
bool SafeEngineName(std::string const& name)
{
    if (name == "_global" || name == "_probe")
        return true;
    if (name.empty() || name.size() > 32 || name[0] == '_')
        return false;
    for (char const c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

// Absolute daily totals, so a row written twice is harmless. No VALUES() (deprecated in MySQL 8.0.20+) and no
// "AS new" alias (MariaDB rejects it): the values are simply repeated.
void Write(std::vector<BrainUsageRow> const& rows)
{
    for (BrainUsageRow const& row : rows)
    {
        if (!row.day || !SafeEngineName(row.engine))
            continue;
        uint32 const exhausted = row.exhausted ? 1u : 0u;
        PlayerbotsDatabase.Execute(
            "INSERT INTO playerbots_coa_brain_usage (day, engine, requests, failures, input_tokens, output_tokens, "
            "usd_micros, exhausted) VALUES ({}, '{}', {}, {}, {}, {}, {}, {}) ON DUPLICATE KEY UPDATE requests = {}, "
            "failures = {}, input_tokens = {}, output_tokens = {}, usd_micros = {}, exhausted = {}",
            row.day, row.engine, row.requests, row.failures, row.inputTokens, row.outputTokens, row.usdMicros,
            exhausted, row.requests, row.failures, row.inputTokens, row.outputTokens, row.usdMicros, exhausted);
    }
}
}  // namespace

BrainUsageStore& BrainUsageStore::Instance()
{
    static BrainUsageStore instance;
    return instance;
}

void BrainUsageStore::LoadSync()
{
    COA_ASSERT_WORLD_THREAD();
    if (loaded)
        return;

    // The synchronous connection: the async worker may be busy for minutes (item caches at a first start).
    if (!PlayerbotsDatabase.Query(TABLE_CHECK_SQL))
    {
        tableMissing = true;
        loaded = true;
        LOG_ERROR("playerbots.brain",
                  "CoA brain: table playerbots_coa_brain_usage is missing (apply "
                  "data/sql/playerbots/updates/2026_09_30_01_coa_brain_usage.sql); the day's usage is kept in memory "
                  "only");
        return;
    }

    uint32 const day = BrainClock::UtcDayNow();
    std::vector<BrainUsageRow> rows;
    if (QueryResult const result = PlayerbotsDatabase.Query(
            "SELECT engine, requests, failures, input_tokens, output_tokens, usd_micros, exhausted "
            "FROM playerbots_coa_brain_usage WHERE day = {}",
            day))
    {
        do
        {
            Field* fields = result->Fetch();
            BrainUsageRow row;
            row.day = day;
            row.engine = fields[0].Get<std::string>();
            row.requests = fields[1].Get<uint32>();
            row.failures = fields[2].Get<uint32>();
            row.inputTokens = fields[3].Get<uint64>();
            row.outputTokens = fields[4].Get<uint64>();
            row.usdMicros = fields[5].Get<uint64>();
            row.exhausted = fields[6].Get<uint8>() != 0;
            rows.push_back(std::move(row));
        } while (result->NextRow());
    }

    uint64 spent = 0;
    for (BrainUsageRow const& row : rows)
        if (row.engine == "_global")
            spent = row.usdMicros;
    BrainGovernor::Instance().Seed(rows);
    loaded = true;
    LOG_INFO("playerbots.brain", "CoA brain usage: {} rows loaded for UTC day {}, ${:.4f} spent today", rows.size(),
             day, double(spent) / 1e6);
}

void BrainUsageStore::Update(uint64 nowMs)
{
    COA_ASSERT_WORLD_THREAD();
    if (!loaded || tableMissing)
        return;

    if (nowMs >= nextFlushMs)
    {
        nextFlushMs = nowMs + FLUSH_EVERY_MS;
        Write(BrainGovernor::Instance().TakeDirtyRows());
    }

    uint32 const today = BrainClock::UtcDayNow();
    if (retentionDay != today)
    {
        retentionDay = today;
        uint32 const oldest = BrainClock::UtcDay(int64(std::time(nullptr)) - RETENTION_DAYS * 86400);
        PlayerbotsDatabase.Execute("DELETE FROM playerbots_coa_brain_usage WHERE day < {} LIMIT 1000", oldest);
    }
}

void BrainUsageStore::Flush()
{
    COA_ASSERT_WORLD_THREAD();
    if (!loaded || tableMissing)
        return;
    Write(BrainGovernor::Instance().TakeDirtyRows());
}
