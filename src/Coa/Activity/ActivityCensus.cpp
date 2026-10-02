/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityCensus.h"
#include "ActivityCandidates.h"
#include "CoaThread.h"
#include "Player.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include <algorithm>

namespace
{
// Nearest rank on sorted counts: the smallest value with at least `percent` % of the samples at or below it.
uint32 NearestRank(std::vector<uint16> const& sorted, uint32 percent)
{
    if (sorted.empty())
        return 0;
    std::size_t rank = (sorted.size() * percent + 99) / 100;
    rank = std::clamp<std::size_t>(rank, 1, sorted.size());
    return sorted[rank - 1];
}
}  // namespace

void ActivityCensus::Update(uint32 nowUnix, std::unordered_set<uint32> const& held)
{
    COA_ASSERT_WORLD_THREAD();
    if (lastUnix && nowUnix < lastUnix + ActivityThreshold::CENSUS_INTERVAL_S)
        return;
    lastUnix = nowUnix;

    Sample sample;
    sample.unixTime = nowUnix;
    sample.hour = uint8(Acore::Time::GetHours(Seconds(nowUnix)));
    uint32 online = 0;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (!bot || !bot->IsInWorld())
            continue;
        ++online;

        uint8 const team = uint8(bot->GetTeamId());
        if (team > 1 || held.count(bot->GetGUID().GetCounter()) || !ActivityCandidates::IsFree(bot) || bot->IsInCombat())
            continue;
        if (bot->GetLevel() == 60 && sample.exactLevel60[team] < 0xFFFF)
            ++sample.exactLevel60[team];
        uint16& count = sample.free[team][ActivityBracket(bot->GetLevel())];
        if (count < 0xFFFF)
            ++count;
        if (bot->GetLevel() >= raidMinLevel && sample.raidFree[team] < 0xFFFF)
            ++sample.raidFree[team];
    }
    sample.online = uint16(std::min<uint32>(online, 0xFFFF));

    if (ring.size() < ActivityThreshold::CENSUS_SAMPLES)
        ring.push_back(sample);
    else
    {
        ring[next] = sample;
        next = (next + 1) % ActivityThreshold::CENSUS_SAMPLES;
    }
}

std::vector<std::string> ActivityCensus::Report(uint32 nowUnix, uint32 hours) const
{
    hours = std::clamp<uint32>(hours, 1, 168);
    uint32 const since = nowUnix > hours * 3600 ? nowUnix - hours * 3600 : 0;

    std::vector<Sample const*> kept;
    Sample const* newest = nullptr;
    for (Sample const& sample : ring)
    {
        if (sample.unixTime < since)
            continue;
        kept.push_back(&sample);
        if (!newest || sample.unixTime > newest->unixTime)
            newest = &sample;
    }
    if (kept.empty())
        return {"no census sample yet (one every 10 minutes while activities run)"};

    std::vector<std::string> lines;
    static char const* const Teams[2] = {"alliance", "horde"};
    for (uint8 team = 0; team < 2; ++team)
    {
        for (uint32 band = 0; band < ActivityThreshold::LEVEL_BANDS; ++band)
        {
            std::vector<uint16> counts;
            counts.reserve(kept.size());
            for (Sample const* sample : kept)
                counts.push_back(sample->free[team][band]);
            std::sort(counts.begin(), counts.end());
            lines.push_back(Acore::StringFormat("{} {}-{}: p10 {}, p50 {}, max {}", Teams[team], band * 10 + 1,
                                                band * 10 + 10, NearestRank(counts, 10), NearestRank(counts, 50),
                                                uint32(counts.back())));
        }
    }
    for (uint8 team = 0; team < 2; ++team)
    {
        std::vector<uint16> counts;
        counts.reserve(kept.size());
        for (Sample const* sample : kept)
            counts.push_back(sample->raidFree[team]);
        std::sort(counts.begin(), counts.end());
        std::vector<uint16> const hourCounts = HourCounts(team, nowUnix);
        std::optional<uint32> const hourP10 = RaidPoolP10(team, nowUnix);
        lines.push_back(Acore::StringFormat("{} raid pool (level {}+): p10 {}, p50 {}, max {}; this hour p10 {} "
                                            "over {} samples",
                                            Teams[team], uint32(raidMinLevel), NearestRank(counts, 10),
                                            NearestRank(counts, 50), uint32(counts.back()),
                                            hourP10 ? std::to_string(*hourP10) : std::string("-"), hourCounts.size()));
    }
    lines.push_back(Acore::StringFormat("census: {} samples over {} h (every 10 min), {} bots online now", kept.size(),
                                        hours, uint32(newest->online)));
    return lines;
}

std::vector<uint16> ActivityCensus::HourCounts(uint8 team, uint32 nowUnix) const
{
    std::vector<uint16> counts;
    if (team > 1)
        return counts;
    uint8 const hour = uint8(Acore::Time::GetHours(Seconds(nowUnix)));
    for (Sample const& sample : ring)
        if (sample.hour == hour)
            counts.push_back(sample.raidFree[team]);
    std::sort(counts.begin(), counts.end());
    return counts;
}

std::optional<uint32> ActivityCensus::RaidPoolP10(uint8 team, uint32 nowUnix) const
{
    std::vector<uint16> const counts = HourCounts(team, nowUnix);
    if (counts.size() < ActivityThreshold::CENSUS_MIN_HOUR_SAMPLES)
        return std::nullopt;
    return NearestRank(counts, 10);
}

void ActivityCensus::Clear()
{
    ring.clear();
    next = 0;
    lastUnix = 0;
}

bool ActivityCensus::HasHistory(uint32 nowUnix, uint32 hours) const
{
    uint32 const duration = std::clamp<uint32>(hours, 1, 168) * 3600;
    if (nowUnix < duration)
        return false;
    uint32 const since = nowUnix - duration;
    std::vector<uint32> times;
    for (Sample const& sample : ring)
        if (sample.unixTime >= since && sample.unixTime <= nowUnix)
            times.push_back(sample.unixTime);
    std::sort(times.begin(), times.end());
    uint32 const interval = ActivityThreshold::CENSUS_INTERVAL_S;
    if (times.empty() || times.front() > since + interval || times.back() + interval < nowUnix)
        return false;
    for (std::size_t i = 1; i < times.size(); ++i)
        if (times[i] - times[i - 1] > 2 * interval)
            return false;
    return times.size() >= duration / interval - 1;
}

std::optional<uint32> ActivityCensus::P10ExactLevel60(uint8 team, uint32 nowUnix, uint32 hours) const
{
    if (team > 1 || !HasHistory(nowUnix, hours))
        return std::nullopt;
    uint32 const since = nowUnix - std::clamp<uint32>(hours, 1, 168) * 3600;
    std::vector<uint16> counts;
    for (Sample const& sample : ring)
        if (sample.unixTime >= since && sample.unixTime <= nowUnix)
            counts.push_back(sample.exactLevel60[team]);
    std::sort(counts.begin(), counts.end());
    return NearestRank(counts, 10);
}

std::vector<std::string> ActivityCensus::SelfTest()
{
    ActivityCensus census;
    uint32 const now = 2000000;
    std::vector<std::string> lines;
    auto check = [&](bool ok, char const* name)
    {
        lines.push_back(std::string(ok ? "PASS census " : "FAIL census ") + name);
    };
    check(!census.HasHistory(now, 168) && !census.P10ExactLevel60(0, now, 168), "empty history stays closed");
    for (uint32 i = 0; i < ActivityThreshold::CENSUS_SAMPLES; ++i)
    {
        Sample sample;
        sample.unixTime = now - (ActivityThreshold::CENSUS_SAMPLES - 1 - i) * ActivityThreshold::CENSUS_INTERVAL_S;
        sample.exactLevel60 = {20, 10};
        census.ring.push_back(sample);
    }
    check(census.HasHistory(now, 168) && census.P10ExactLevel60(0, now, 168) == 20 &&
          census.P10ExactLevel60(1, now, 168) == 10, "full-week coverage and exact-level p10");
    check(!census.P10ExactLevel60(2, now, 168), "neutral team rejected");
    auto const saved = census.ring[500];
    census.ring.erase(census.ring.begin() + 500, census.ring.begin() + 504);
    check(!census.HasHistory(now, 168), "historical gaps keep the gate closed");
    census.ring.clear();
    census.ring.push_back(saved);
    check(!census.HasHistory(now, 168), "one old sample is never a week of evidence");
    return lines;
}
