/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYSTORE_H
#define PLAYERBOTS_ACTIVITYSTORE_H

#include "ActivityTypes.h"
#include "Define.h"
#include <string>
#include <unordered_map>
#include <vector>

// A member row whose CoA ruleset was changed and never put back (the server stopped hard while it ran).
struct ActivityRestoreRow
{
    uint32 guid = 0;
    uint32 prevRuleset = 0;
};

// The activity log: playerbots_coa_activity and playerbots_coa_activity_member (data/sql/playerbots/updates/
// 2026_09_30_03_coa_activity.sql). World thread only. Writes are async PlayerbotsDatabase.Execute with the whole
// statement built by Acore::StringFormat (integers only, plus `summary` and `target` through EscapeString); the
// sync queries run at start only. No statement touches a table before CheckTables saw both (a missing table would
// abort the server). Nothing runs while TablesReady() is false.
class ActivityStore
{
public:
    static ActivityStore& Instance();

    // Sync. Both tables exist (information_schema). Sets TablesReady().
    bool CheckTables();
    bool TablesReady() const { return tablesReady; }

    // Sync: SELECT COALESCE(MAX(id), 0) FROM playerbots_coa_activity. Ids are then allocated in memory.
    uint32 LoadMaxId();

    // Crash recovery, at start: live rows become aborted (outcome Restart, ended now), their open member rows get
    // left_at, and restore attempts older than LogKeepDays are given up (restored = 1). Async.
    void AbortLiveRows(uint32 nowUnix, uint32 logKeepDays);
    // Sync, at start, after AbortLiveRows was sent (one async worker: the SELECT sees the updates): every member row
    // with restored = 0 and prev_ruleset <> 0, newest first per guid (one row per guid kept).
    std::vector<ActivityRestoreRow> LoadUnrestored();

    // An activity that started (Begin): its row with the live state.
    void InsertActivity(uint32 id, ActivityKindId kind, uint8 team, std::string const& target, uint32 leaderGuid,
                        ActivityPhase phase, uint32 startedUnix);
    // A phase change of a live activity.
    void UpdatePhase(uint32 id, ActivityPhase phase);
    // The leader changed (group kinds: a real player took the lead at the end, or the kind handed it over).
    void UpdateLeader(uint32 id, uint32 leaderGuid);
    // The end: state 10 (Success) or 11, ended, outcome, counts, summary (<= 255 bytes, escaped).
    void EndActivity(uint32 id, ActivityOutcome outcome, uint32 endedUnix, uint32 bots, uint32 players,
                     std::string const& summary);

    // A member row (restored = 1: nothing to restore yet).
    void InsertMember(uint32 activityId, uint32 guid, bool isBot, ActivityRole role, uint32 joinedUnix);
    // The member left (or the activity ended while it was in).
    void MemberLeft(uint32 activityId, uint32 guid, uint32 leftUnix);
    // ApplyWarMode succeeded: prev_ruleset and restored = 0.
    void SetPrevRuleset(uint32 activityId, uint32 guid, uint32 prevRuleset);
    // RestoreRuleset succeeded: every row of that guid with restored = 0.
    void MarkRestored(uint32 guid);

    // Every RETENTION_INTERVAL_MS: an async MIN(id) of the activities started within LogKeepDays (nextId when none),
    // then, in its callback, the member rows (restored = 1) and ended activity rows below it, LIMIT 5000 / 1000.
    void Retention(uint32 nowUnix, uint32 logKeepDays, uint32 nextId);

    // Every director tick: runs the callbacks of the async queries that are ready.
    void ProcessCallbacks();

    // Director stop or shutdown: drops the callbacks in flight.
    void Reset();

private:
    ActivityStore() = default;
    ActivityStore(ActivityStore const&) = delete;
    ActivityStore& operator=(ActivityStore const&) = delete;

    bool tablesReady = false;
    uint32 giveUpBeforeUnix = 0;  // AbortLiveRows' cutoff: older unrestored rows are given up (LoadUnrestored too)
    // The QueryCallbackProcessor is a file-static of ActivityStore.cpp (world thread only), which keeps the database
    // headers out of this one.
};

#endif
