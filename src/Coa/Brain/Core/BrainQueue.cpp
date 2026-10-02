/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainQueue.h"
#include "BrainSink.h"
#include "CoaTime.h"
#include <utility>

namespace
{
constexpr uint32 LOWER_LANE_EVERY = 5;       // every 5th pop may serve a lower lane
constexpr uint64 LOWER_LANE_WAIT_MS = 2000;  // ... whose oldest job waited longer than this
}  // namespace

BrainQueue::BrainQueue(uint32 playerCapacity, uint32 organizerCapacity, uint32 ambientCapacity)
    : capacity{playerCapacity, organizerCapacity, ambientCapacity}
{
}

bool BrainQueue::CanPush(BrainLane lane) const
{
    std::size_t const index = std::size_t(lane);
    if (index >= lanes.size())
        return false;

    std::lock_guard<std::mutex> guard(lock);
    if (closed)
        return false;
    if (lane == BrainLane::Ambient)
        return capacity[index] > 0;
    return lanes[index].size() < capacity[index];
}

bool BrainQueue::Push(BrainQueued&& item, std::vector<BrainQueued>& evicted)
{
    std::size_t const index = std::size_t(item.job.lane);
    if (index >= lanes.size())
        return false;

    {
        std::lock_guard<std::mutex> guard(lock);
        if (closed || !capacity[index])
            return false;

        std::deque<BrainQueued>& lane = lanes[index];
        if (lane.size() >= capacity[index])
        {
            if (item.job.lane != BrainLane::Ambient)
                return false;
            evicted.push_back(std::move(lane.front()));
            lane.pop_front();
        }

        lane.push_back(std::move(item));
        depth[index].store(uint32(lane.size()), std::memory_order_relaxed);
    }

    wake.notify_one();
    return true;
}

bool BrainQueue::Pop(BrainQueued& out)
{
    std::unique_lock<std::mutex> guard(lock);
    wake.wait(guard,
              [this]
              {
                  if (closed)
                      return true;
                  for (std::deque<BrainQueued> const& lane : lanes)
                      if (!lane.empty())
                          return true;
                  return false;
              });
    if (closed)
        return false;

    std::size_t chosen = lanes.size();
    if (++pops % LOWER_LANE_EVERY == 0)
    {
        uint64 const now = CoaTime::NowMs();
        for (std::size_t index = std::size_t(BrainLane::Organizer); index < lanes.size(); ++index)
        {
            std::deque<BrainQueued> const& lane = lanes[index];
            if (!lane.empty() && now > lane.front().enqueuedMs && now - lane.front().enqueuedMs > LOWER_LANE_WAIT_MS)
            {
                chosen = index;
                break;
            }
        }
    }
    if (chosen == lanes.size())
    {
        for (std::size_t index = 0; index < lanes.size(); ++index)
        {
            if (!lanes[index].empty())
            {
                chosen = index;
                break;
            }
        }
    }
    if (chosen == lanes.size())
        return false;

    out = std::move(lanes[chosen].front());
    lanes[chosen].pop_front();
    depth[chosen].store(uint32(lanes[chosen].size()), std::memory_order_relaxed);
    return true;
}

bool BrainQueue::Remove(uint64 jobId, BrainQueued& removed)
{
    std::lock_guard<std::mutex> guard(lock);
    for (std::size_t index = 0; index < lanes.size(); ++index)
    {
        std::deque<BrainQueued>& lane = lanes[index];
        for (auto it = lane.begin(); it != lane.end(); ++it)
        {
            if (it->job.id != jobId)
                continue;
            removed = std::move(*it);
            lane.erase(it);
            depth[index].store(uint32(lane.size()), std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

std::vector<BrainQueued> BrainQueue::Close()
{
    std::vector<BrainQueued> waiting;
    {
        std::lock_guard<std::mutex> guard(lock);
        closed = true;
        for (std::size_t index = 0; index < lanes.size(); ++index)
        {
            for (BrainQueued& item : lanes[index])
                waiting.push_back(std::move(item));
            lanes[index].clear();
            depth[index].store(0, std::memory_order_relaxed);
        }
    }
    wake.notify_all();
    return waiting;
}
