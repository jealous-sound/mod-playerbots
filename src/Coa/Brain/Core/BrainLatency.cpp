/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainLatency.h"
#include <algorithm>

void BrainLatency::Add(uint32 ms)
{
    samples[next] = ms;
    next = (next + 1) % uint32(samples.size());
    if (count < samples.size())
        ++count;
}

uint32 BrainLatency::Percentile(uint32 percentile) const
{
    if (!count)
        return 0;

    // Until the ring is full the filled part is [0, count); afterwards every slot holds a sample.
    std::array<uint32, 256> sorted = samples;
    uint32 const n = count;
    uint32 const rank = std::min(n - 1, n * std::min(percentile, 100u) / 100);
    std::nth_element(sorted.begin(), sorted.begin() + rank, sorted.begin() + n);
    return sorted[rank];
}

void BrainLatency::Clear()
{
    samples.fill(0);
    next = 0;
    count = 0;
}
