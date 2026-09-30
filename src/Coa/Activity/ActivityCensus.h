/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYCENSUS_H
#define PLAYERBOTS_ACTIVITYCENSUS_H

#include "ActivityTypes.h"
#include "Define.h"
#include <array>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// How many bots are free for an activity, by faction and level band, every 10 minutes while the director runs
// (M5's gate for chat-organized raids). RAM only: a restart starts over. World thread only.
class ActivityCensus
{
public:
    // One sample: bots counted per [team][band]; band = ActivityBracket(level) (0: 1-10 ... 5: 51+).
    struct Sample
    {
        uint32 unixTime = 0;
        std::array<std::array<uint16, ActivityThreshold::LEVEL_BANDS>, 2> free{};
        uint16 online = 0;  // random bots online (free or not)
        std::array<uint16, 2> raidFree{};  // M5: free bots per team at Raid.MinLevel or above (the raid pool)
        uint8 hour = 0;                    // M5: server local hour of day when taken (Acore::Time::GetHours)
    };

    // The director, every tick: takes a sample when CENSUS_INTERVAL_S passed since the last one. Free is
    // ActivityCandidates::IsFree and not in `held`.
    void Update(uint32 nowUnix, std::unordered_set<uint32> const& held);

    // .playerbots activity census [hours]: the samples of the last `hours` (1..168, default 24), per faction and
    // band: "<faction> <band>: p10 <n>, p50 <n>, max <n>", then (M5) "<faction> raid pool (level <n>+): p10 <n>, p50
    // <n>, max <n>; this hour p10 <n|-> over <k> samples", then "census: <n> samples over <h> h (every 10 min),
    // <online> bots online now". "no census sample yet" when empty.
    std::vector<std::string> Report(uint32 nowUnix, uint32 hours) const;

    void Clear();

    // M5. The level the raid pool counts from (Raid.MinLevel); the director sets it at start and on every reload.
    // Samples taken before a change keep the old level.
    void SetRaidMinLevel(uint8 level) { raidMinLevel = level; }

    // M5, the outpost raid's gate: the p10 (nearest rank) of raidFree[team] over the samples taken at the current
    // server hour of day (every kept day); nullopt with fewer than CENSUS_MIN_HOUR_SAMPLES of them.
    std::optional<uint32> RaidPoolP10(uint8 team, uint32 nowUnix) const;

private:
    // raidFree[team] of every kept sample taken at nowUnix's hour of day, sorted.
    std::vector<uint16> HourCounts(uint8 team, uint32 nowUnix) const;

    std::vector<Sample> ring;  // at most CENSUS_SAMPLES, oldest overwritten
    uint32 next = 0;           // next slot to write once the ring is full
    uint32 lastUnix = 0;
    uint8 raidMinLevel = 45;
};

#endif
