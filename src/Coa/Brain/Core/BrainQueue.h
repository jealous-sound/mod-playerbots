/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINQUEUE_H
#define PLAYERBOTS_BRAINQUEUE_H

#include "BrainTypes.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

class BrainSink;

// A job waiting for a worker, with where its result goes and what Admit reserved for it.
struct BrainQueued
{
    BrainJob job;
    std::shared_ptr<BrainSink> sink;
    uint64 reservedMicros = 0;  // released through BrainGovernor::Settle when the job ends, whatever the end
    uint64 enqueuedMs = 0;
};

// Three bounded FIFO lanes shared by Submit (any game thread) and the workers. Pop serves Player first, then
// Organizer, then Ambient; every 5th pop takes the oldest job of a lower lane instead when it has waited more
// than 2 s, so a busy Player lane cannot starve the others. One mutex, held only to move jobs.
class BrainQueue
{
public:
    BrainQueue(uint32 playerCapacity = 64, uint32 organizerCapacity = 64, uint32 ambientCapacity = 128);

    // Any thread. Whether Push would take a job of that lane now (the Ambient lane always can: it evicts).
    bool CanPush(BrainLane lane) const;

    // Any thread. False when the lane is full (Player, Organizer) or the queue is closed. A full Ambient lane
    // evicts its oldest job into `evicted` (the caller posts it a Cancelled result and settles its reservation).
    bool Push(BrainQueued&& item, std::vector<BrainQueued>& evicted);

    // Worker thread. Blocks until a job is there or the queue is closed; false once closed.
    bool Pop(BrainQueued& out);

    // Any thread (IBrain::Cancel). Removes a waiting job; false when no lane holds it.
    bool Remove(uint64 jobId, BrainQueued& removed);

    // World thread (shutdown). Wakes every worker; Pop returns false from now on. Returns what still waited.
    std::vector<BrainQueued> Close();

    uint32 Depth(BrainLane lane) const { return depth[size_t(lane)].load(std::memory_order_relaxed); }

private:
    BrainQueue(BrainQueue const&) = delete;
    BrainQueue& operator=(BrainQueue const&) = delete;

    mutable std::mutex lock;
    std::condition_variable wake;
    std::array<std::deque<BrainQueued>, size_t(BrainLane::Count)> lanes;  // guarded by lock
    std::array<uint32, size_t(BrainLane::Count)> capacity;
    std::array<std::atomic<uint32>, size_t(BrainLane::Count)> depth{};
    uint32 pops = 0;      // guarded by lock: every 5th pop may serve a lower lane
    bool closed = false;  // guarded by lock
};

#endif
