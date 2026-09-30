/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINSINK_H
#define PLAYERBOTS_BRAINSINK_H

#include "BrainTypes.h"
#include "LockedQueue.h"
#include <atomic>
#include <vector>

// Where the results of one consumer's jobs land: the voice director and BrainAdmin each own one. Workers post;
// the consumer drains on the world thread (from CoaWorldScript::OnUpdate) and drops results it no longer waits
// for (unknown tag, other epoch). Every queued job holds a shared_ptr to its sink, so a worker detached at
// shutdown never posts into freed memory.
class BrainSink
{
public:
    explicit BrainSink(uint32 capacity = 1024);

    // Any thread. False, with the result dropped and counted, when `capacity` results already wait or are being
    // posted; false and uncounted once closed.
    bool Post(BrainResult&& result);

    // The consumer's thread. Appends every waiting result to `out`, oldest first.
    void Drain(std::vector<BrainResult>& out);

    bool Empty() const { return size.load(std::memory_order_acquire) == 0; }
    uint64 Dropped() const { return dropped.load(std::memory_order_relaxed); }

    // The consumer's thread (shutdown). Frees what waits; later posts are dropped uncounted.
    void Close();
    bool Closed() const { return closed.load(std::memory_order_relaxed); }

private:
    BrainSink(BrainSink const&) = delete;
    BrainSink& operator=(BrainSink const&) = delete;

    LockedQueue<BrainResult> queue;
    std::atomic<uint32> size{0};
    std::atomic<uint64> dropped{0};
    std::atomic<bool> closed{false};
    uint32 const capacity;
};

#endif
