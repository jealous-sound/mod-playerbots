/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINCACHE_H
#define PLAYERBOTS_BRAINCACHE_H

#include "BrainTypes.h"
#include <array>
#include <atomic>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Answers to repeated situations ("salut", "gg", "lol" in the same kind of place), so the brain is not asked the
// same thing again for CacheTtlSeconds. It never stores line ids: the lines offered change every call. A Choice
// question whose options carry groups (the line question: a group is a reply name) is stored as a distribution
// over groups; a hit samples one group from it, and the caller draws a line of that group itself. Questions that
// are not cacheable (who) are left out: the caller treats them as unanswered. 16 shards, one mutex each; LRU per
// shard; process-wide.
class BrainCache
{
public:
    static BrainCache& Instance();

    // World thread (BrainService::Apply). A capacity of 0 turns the cache off and empties it.
    void Configure(uint32 capacity, uint32 ttlSeconds);

    // Any thread (Submit). A hit fills `out`: answers, engine, calibrated, fromCache = true, error None. About
    // one lookup in 20 misses on purpose so the entry is refreshed. Expired entries miss and are removed.
    bool Lookup(std::string const& key, uint64 nowMs, BrainResult& out);

    // Worker thread, after a successful call. Keeps the valid answers of cacheable questions; grouped Choice
    // answers become group distributions (the probabilities of a group's options summed, renormalized).
    void Store(std::string const& key, BrainJob const& job, BrainResult const& result, uint64 nowMs);

    void Clear();

    uint64 Hits() const { return hits.load(std::memory_order_relaxed); }
    uint64 Misses() const { return misses.load(std::memory_order_relaxed); }
    uint32 Size() const;

private:
    BrainCache() = default;
    BrainCache(BrainCache const&) = delete;
    BrainCache& operator=(BrainCache const&) = delete;

    struct Entry
    {
        std::vector<BrainAnswer> answers;  // grouped answers: `dist` holds group -> p, `choice` is empty
        std::vector<bool> grouped;         // per answer
        std::string engine;
        std::string providerModel;
        bool calibrated = false;
        uint64 storedMs = 0;
        std::list<std::string>::iterator lru;
    };

    struct Shard
    {
        mutable std::mutex lock;
        std::unordered_map<std::string, Entry> entries;
        std::list<std::string> lru;  // most recent first
    };

    Shard& ShardFor(std::string const& key);

    std::array<Shard, 16> shards;
    std::atomic<uint32> capacityPerShard{0};
    std::atomic<uint32> ttlMs{600000};
    std::atomic<uint64> hits{0};
    std::atomic<uint64> misses{0};
};

#endif
