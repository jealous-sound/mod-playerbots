/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainDecisionRing.h"
#include "CoaThread.h"
#include <utility>

BrainDecisionRing& BrainDecisionRing::Instance()
{
    static BrainDecisionRing instance;
    return instance;
}

void BrainDecisionRing::Add(BrainDecisionEntry&& entry)
{
    COA_ASSERT_WORLD_THREAD();
    entries[next] = std::move(entry);
    next = (next + 1) % Capacity;
    if (count < Capacity)
        ++count;
}

std::vector<BrainDecisionEntry> BrainDecisionRing::ForBot(uint64 botGuid, uint32 max) const
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<BrainDecisionEntry> out;
    for (uint32 i = 0; i < count && out.size() < max; ++i)
    {
        BrainDecisionEntry const& entry = entries[(next + Capacity - 1 - i) % Capacity];
        if (entry.botGuid == botGuid)
            out.push_back(entry);
    }
    return out;
}

std::vector<BrainDecisionEntry> BrainDecisionRing::Last(uint32 max) const
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<BrainDecisionEntry> out;
    for (uint32 i = 0; i < count && out.size() < max; ++i)
        out.push_back(entries[(next + Capacity - 1 - i) % Capacity]);
    return out;
}

void BrainDecisionRing::Clear()
{
    COA_ASSERT_WORLD_THREAD();
    for (BrainDecisionEntry& entry : entries)
        entry = BrainDecisionEntry{};
    next = 0;
    count = 0;
}
