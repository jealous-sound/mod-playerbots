/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotMemory.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

namespace
{
constexpr std::size_t NAME_MAX_CHARACTERS = 24;  // other_name is VARCHAR(24) utf8mb4: characters, not bytes
constexpr uint32 MERGE_WINDOW = 600;
constexpr uint32 RECENT_WINDOW = 7 * 86400;
// Decay truncates toward 0, so re-basing on every write would shave a tenth of a point off every axis per write.
// Axes are re-based at most once an hour; a delta added in between is decayed from decayedAt (under 1 % off).
constexpr uint32 REBASE_SECONDS = 3600;

// The first 24 UTF-8 characters of `name`.
std::string CutName(std::string const& name)
{
    std::size_t characters = 0;
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        if ((static_cast<unsigned char>(name[i]) & 0xC0) == 0x80)
            continue;
        if (characters++ == NAME_MAX_CHARACTERS)
            return name.substr(0, i);
    }
    return name;
}

// Higher is kept longer (decayed values).
int32 KeepScore(MemoryRelation const& relation, uint32 now, MemoryDecay const& decay)
{
    auto const axis = [&](MemoryAxis a)
    { return int32(decay.Apply(relation.axes[std::size_t(a)], a, relation.decayedAt, now)); };
    int32 score = std::abs(axis(MemoryAxis::Affinity)) + axis(MemoryAxis::Rivalry) + axis(MemoryAxis::Grudge) +
                  std::abs(axis(MemoryAxis::Trust)) / 2 + axis(MemoryAxis::Familiarity) / 4;
    if (relation.lastSeen + RECENT_WINDOW >= now)
        score += 200;
    if (relation.otherIsBot)
        score -= 300;
    return score;
}
}  // namespace

MemoryRelation* BotMemory::Find(uint32 otherGuid)
{
    auto const found = relations.find(otherGuid);
    return found != relations.end() ? &found->second : nullptr;
}

MemoryRelation const* BotMemory::Find(uint32 otherGuid) const
{
    auto const found = relations.find(otherGuid);
    return found != relations.end() ? &found->second : nullptr;
}

MemoryRelation& BotMemory::Touch(uint32 otherGuid, MemoryOther const& other, uint32 now, uint32 maxRelations,
                                 MemoryDecay const& decay)
{
    auto found = relations.find(otherGuid);
    if (found == relations.end())
    {
        if (maxRelations && relations.size() >= maxRelations)
        {
            auto victim = relations.end();
            int32 lowest = std::numeric_limits<int32>::max();
            for (auto it = relations.begin(); it != relations.end(); ++it)
            {
                int32 const score = KeepScore(it->second, now, decay);
                if (score < lowest)
                {
                    lowest = score;
                    victim = it;
                }
            }
            if (victim != relations.end())
            {
                uint32 const evicted = victim->first;
                dirty.erase(evicted);
                if (std::find(deleted.begin(), deleted.end(), evicted) == deleted.end())
                    deleted.push_back(evicted);
                relations.erase(victim);
            }
        }

        MemoryRelation created;
        created.otherGuid = otherGuid;
        created.firstSeen = now;
        created.decayedAt = now;
        found = relations.emplace(otherGuid, std::move(created)).first;
    }

    MemoryRelation& relation = found->second;
    if (now >= relation.decayedAt + REBASE_SECONDS)
    {
        for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
            relation.axes[i] = decay.Apply(relation.axes[i], MemoryAxis(i), relation.decayedAt, now);
        relation.decayedAt = now;
    }
    relation.lastSeen = std::max(relation.lastSeen, now);

    // An unknown identity (empty name) keeps the stored snapshot.
    if (!other.name.empty())
    {
        relation.otherIsBot = other.isBot;
        relation.otherAccount = other.isBot ? 0 : other.account;
        relation.otherName = CutName(other.name);
    }

    dirty.insert(otherGuid);
    return relation;
}

void BotMemory::AddAxis(MemoryRelation& relation, MemoryAxis axis, int32 delta)
{
    if (!delta)
        return;
    std::size_t const i = std::size_t(axis);
    relation.axes[i] = MemoryClampAxis(axis, int32(relation.axes[i]) + delta);
    dirty.insert(relation.otherGuid);
}

void BotMemory::AddCounter(MemoryRelation& relation, MemoryCounter counter, uint32 n)
{
    if (!n)
        return;
    uint16& value = relation.counters[std::size_t(counter)];
    value = uint16(std::min<uint32>(uint32(value) + n, MEMORY_COUNTER_MAX));
    dirty.insert(relation.otherGuid);
}

bool BotMemory::AddEvent(MemoryEvent&& event, uint32 maxLoaded)
{
    if (MemoryEventMerges(event.kind))
    {
        // Newest first; events are appended in time order, so the scan stops at the first one out of the window.
        for (auto it = events.rbegin(); it != events.rend(); ++it)
        {
            if (it->ts + MERGE_WINDOW <= event.ts)
                break;
            if (it->kind == event.kind && it->otherGuid == event.otherGuid)
                return false;
        }
    }

    if (event.detail.size() > MEMORY_DETAIL_MAX)
        event.detail.resize(MEMORY_DETAIL_MAX);

    unsaved.push_back(event);
    events.push_back(std::move(event));
    while (maxLoaded && events.size() > maxLoaded)
        events.pop_front();
    return true;
}

std::size_t BotMemory::Forget(uint32 otherGuid)
{
    std::size_t removed = relations.erase(otherGuid);
    dirty.erase(otherGuid);
    auto const about = [otherGuid](MemoryEvent const& event) { return event.otherGuid == otherGuid; };
    removed += std::erase_if(events, about);
    std::erase_if(unsaved, about);
    return removed;
}

std::size_t BotMemory::ForgetAccount(uint32 account)
{
    if (!account)
        return 0;

    std::size_t removed = 0;
    for (auto it = relations.begin(); it != relations.end();)
    {
        if (it->second.otherAccount == account)
        {
            dirty.erase(it->first);
            it = relations.erase(it);
            ++removed;
        }
        else
            ++it;
    }
    auto const about = [account](MemoryEvent const& event) { return event.otherAccount == account; };
    removed += std::erase_if(events, about);
    std::erase_if(unsaved, about);
    return removed;
}

void BotMemory::Clear()
{
    relations.clear();
    events.clear();
    unsaved.clear();
    dirty.clear();
    deleted.clear();
    eventsSinceTrim = 0;
    lastRevengeCallAt = 0;
}
