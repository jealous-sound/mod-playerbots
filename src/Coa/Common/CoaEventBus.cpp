/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaEventBus.h"
#include "CoaTime.h"
#include <algorithm>
#include <iterator>

CoaEventBus& CoaEventBus::Instance()
{
    static CoaEventBus instance;
    return instance;
}

void CoaEventBus::Configure(uint32 newCapacity)
{
    // A smaller capacity than what is queued keeps the queue: the posts are refused until the next drain.
    capacity.store(std::max<uint32>(1, newCapacity), std::memory_order_relaxed);
}

bool CoaEventBus::Post(CoaEvent&& event)
{
    if (!IsEnabled())
        return false;

    if (!event.postedMs)
        event.postedMs = CoaTime::NowMs();

    std::lock_guard<std::mutex> guard(lock);
    // Close() may have run between IsEnabled() and the lock.
    if (closed.load(std::memory_order_relaxed))
        return false;

    if (queue.size() >= capacity.load(std::memory_order_relaxed))
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    queue.push_back(std::move(event));
    posted.fetch_add(1, std::memory_order_relaxed);
    pending.store(uint32(queue.size()), std::memory_order_relaxed);
    return true;
}

void CoaEventBus::Drain(std::vector<CoaEvent>& out)
{
    std::lock_guard<std::mutex> guard(lock);
    if (queue.empty())
        return;

    // The swap hands the consumer's emptied vector back to the queue, so neither side reallocates every tick.
    if (out.empty())
        out.swap(queue);
    else
    {
        out.insert(out.end(), std::make_move_iterator(queue.begin()), std::make_move_iterator(queue.end()));
        queue.clear();
    }

    pending.store(0, std::memory_order_relaxed);
}

void CoaEventBus::Close()
{
    std::vector<CoaEvent> freed;
    {
        std::lock_guard<std::mutex> guard(lock);
        closed.store(true, std::memory_order_relaxed);
        freed.swap(queue);
        pending.store(0, std::memory_order_relaxed);
    }
}
