/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINUSAGESTORE_H
#define PLAYERBOTS_BRAINUSAGESTORE_H

#include "Define.h"

// Persists BrainGovernor's daily totals in playerbots_coa_brain_usage so the caps survive restarts. World thread
// only; the database headers stay in BrainUsageStore.cpp. Nothing runs until the brain first starts.
class BrainUsageStore
{
public:
    static BrainUsageStore& Instance();

    // World thread, before the brain first starts (startup, a .reload config or .playerbots brain on that turns it
    // on). Synchronous, on PlayerbotsDatabase's synchronous connection: first asks information_schema whether the
    // table exists (a query on a missing table aborts the server); missing -> one LOG_ERROR naming
    // data/sql/playerbots/updates/2026_09_30_01_coa_brain_usage.sql, and the usage lives in memory only. Else
    // reads today's rows and seeds BrainGovernor. Idempotent.
    void LoadSync();

    // World thread, every CoaWorldScript::OnUpdate once the brain ever ran. Every 60 s: the rows BrainGovernor
    // reports dirty, as async upserts. Once per UTC day: deletes rows older than 90 days (async, LIMIT 1000).
    void Update(uint64 nowMs);

    // World thread, WORLDHOOK_ON_SHUTDOWN after the brain stopped: the final upsert (async; PlayerbotsDatabase's
    // Close in OnDatabasesClosing drains it).
    void Flush();

    bool Loaded() const { return loaded; }
    bool TableMissing() const { return tableMissing; }

private:
    BrainUsageStore() = default;
    BrainUsageStore(BrainUsageStore const&) = delete;
    BrainUsageStore& operator=(BrainUsageStore const&) = delete;

    bool loaded = false;
    bool tableMissing = false;
    uint64 nextFlushMs = 0;
    uint32 retentionDay = 0;  // UTC day of the last retention delete
};

#endif
