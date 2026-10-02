/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTMEMORY_H
#define PLAYERBOTS_BOTMEMORY_H

#include "Define.h"
#include "MemoryEvent.h"
#include "MemoryTypes.h"
#include <array>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Who the other side of a relation is, as known when it is touched.
struct MemoryOther
{
    uint32 account = 0;  // 0 for bots
    bool isBot = false;  // a random bot account; a player's altbot counts as the player (account kept)
    std::string name;    // cut to 24 characters when stored
    uint8 level = 0;
};

// One row of playerbots_coa_memory_relation, plus facts kept in RAM only.
struct MemoryRelation
{
    uint32 otherGuid = 0;
    uint32 otherAccount = 0;  // other_account: 0 for bots
    bool otherIsBot = false;
    std::string otherName;    // other_name snapshot
    std::array<int16, MEMORY_AXIS_COUNT> axes{};           // as of decayedAt
    std::array<uint16, MEMORY_COUNTER_COUNT> counters{};
    uint32 firstSeen = 0;
    uint32 lastSeen = 0;
    uint32 decayedAt = 0;

    // RAM only, never stored.
    uint32 lastMetAt = 0;          // Meetings/Met dedupe: 30 minutes (set to lastSeen at load)
    uint32 lastGroupedAt = 0;      // Grouped dedupe: 30 minutes
    uint32 lastKilledMeAt = 0;     // newest KilledBy (from the loaded events, then live)
    uint32 bgGrudgeUntil = 0;      // a battleground kill of the bot: a grudge until then (BattlegroundKills = 1)
    uint32 wordDay = 0;            // UTC day (unix time / 86400) of wordUsed
    uint16 wordUsed = 0;           // |affinity| moved by words and emotes that day, 1/10 points
    uint8 fledStreak = 0;          // duels in a row they fled (from the loaded events, then live)
    uint32 lastFledAt = 0;         // newest DuelFled (from the loaded events, then live)
    uint32 lastRevengeCallAt = 0;  // Revenge: the last guild call about them
};

// One bot's memory. Owned by CoaMemoryMgr through a shared_ptr and touched on the world thread only; the bot's map
// thread reads a MemoryView built from it, never this. Fields are public for the store's own files (manager,
// repository, retrieval, admin); nothing outside src/Coa/Memory includes this header.
class BotMemory
{
public:
    explicit BotMemory(uint32 botGuid) : botGuid(botGuid) {}

    uint32 BotGuid() const { return botGuid; }

    MemoryRelation* Find(uint32 otherGuid);
    MemoryRelation const* Find(uint32 otherGuid) const;

    // The relation with `otherGuid`, created when missing (first_seen = now). When the bot already keeps
    // maxRelations, the one with the lowest keep score is evicted first (see the contract: bots before players,
    // then |affinity| + rivalry + grudge + |trust| / 2 + familiarity / 4, +200 when seen in the last 7 days) and its
    // guid queued in `deleted`. The axes are decayed to `now` (decayedAt = now) when decayedAt is an hour old or more
    // (so truncation does not wear them down write after write): read them through MemoryDecay for decisions.
    // lastSeen = now; the snapshot fields (account, is_bot, name) are refreshed from `other` when its name is known;
    // the relation is marked dirty.
    MemoryRelation& Touch(uint32 otherGuid, MemoryOther const& other, uint32 now, uint32 maxRelations,
                          MemoryDecay const& decay);

    // Clamped add (MemoryClampAxis); marks the relation dirty.
    void AddAxis(MemoryRelation& relation, MemoryAxis axis, int32 delta);
    // Saturating add; marks the relation dirty.
    void AddCounter(MemoryRelation& relation, MemoryCounter counter, uint32 n = 1);

    // Appends to `events` (dropping the oldest past maxLoaded) and to `unsaved`. A merging kind (MemoryEventMerges)
    // about the same other within 600 s of one of that kind in `events` is dropped instead: false.
    bool AddEvent(MemoryEvent&& event, uint32 maxLoaded);

    // RAM only (the caller sends the DELETEs). The relation and every event about `otherGuid`; the number of
    // things removed.
    std::size_t Forget(uint32 otherGuid);
    // RAM only. Every relation and event whose other_account is `account` (never 0).
    std::size_t ForgetAccount(uint32 account);
    // RAM only. Everything, including what was not flushed.
    void Clear();

    // Something to write: dirty relations, unsaved events, or evicted relations to delete.
    bool IsDirty() const { return !dirty.empty() || !unsaved.empty() || !deleted.empty(); }

    std::unordered_map<uint32, MemoryRelation> relations;  // other guid -> relation
    std::deque<MemoryEvent> events;      // loaded and new, oldest first, at most CoaMemory.MaxEventsLoaded
    std::vector<MemoryEvent> unsaved;    // made since the last flush, oldest first
    std::unordered_set<uint32> dirty;    // relations changed since the last flush
    std::vector<uint32> deleted;         // relations evicted since the last flush
    uint32 eventsSinceTrim = 0;          // events inserted since the last per-bot trim
    uint32 lastRevengeCallAt = 0;        // Revenge: 10 minutes between two guild calls of this bot

private:
    uint32 botGuid;
};

#endif
