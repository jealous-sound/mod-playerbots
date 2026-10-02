/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainSink.h"
#include "BrainStats.h"
#include <utility>

BrainSink::BrainSink(uint32 capacity) : capacity(capacity ? capacity : 1) {}

bool BrainSink::Post(BrainResult&& result)
{
    if (closed.load(std::memory_order_acquire))
        return false;

    // Reserve the slot before the result becomes visible, so a Drain never counts an item down before it was
    // counted up (`size` would wrap and refuse every post until the add caught up).
    if (size.fetch_add(1, std::memory_order_acq_rel) >= capacity)
    {
        size.fetch_sub(1, std::memory_order_acq_rel);
        dropped.fetch_add(1, std::memory_order_relaxed);
        BrainStats::Instance().sinkDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    queue.add(std::move(result));
    return true;
}

void BrainSink::Drain(std::vector<BrainResult>& out)
{
    BrainResult result;
    while (queue.next(result))
    {
        out.push_back(std::move(result));
        size.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void BrainSink::Close()
{
    closed.store(true, std::memory_order_release);
    // Counted down one by one like Drain: a post racing the close keeps its own count consistent.
    std::vector<BrainResult> discarded;
    BrainResult result;
    while (queue.next(result))
    {
        discarded.push_back(std::move(result));
        size.fetch_sub(1, std::memory_order_acq_rel);
    }
}
