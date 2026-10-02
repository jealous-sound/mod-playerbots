/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINLATENCY_H
#define PLAYERBOTS_BRAINLATENCY_H

#include "Define.h"
#include <array>

// Round-trip times of one engine's successful calls: the last 256, in milliseconds. Not thread-safe: each
// engine's ring lives in BrainGovernor's per-engine state and is used under the governor's lock.
class BrainLatency
{
public:
    static constexpr uint32 MinSamples = 20;  // below this, callers use a fallback (ConnectTimeoutMs / 2)

    void Add(uint32 ms);

    // percentile in [0, 100]; 0 when there is no sample. Nearest rank over a sorted copy.
    uint32 Percentile(uint32 percentile) const;

    uint32 Count() const { return count; }
    void Clear();

private:
    std::array<uint32, 256> samples{};
    uint32 next = 0;
    uint32 count = 0;
};

#endif
