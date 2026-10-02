/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainCache.h"
#include "CoaHash.h"
#include <algorithm>
#include <functional>
#include <random>
#include <thread>
#include <utility>

namespace
{
constexpr uint32 SHARDS = 16;
constexpr uint32 REFRESH_ONE_IN = 20;  // one lookup in 20 misses on purpose, so entries follow the model

// Worker and game threads each draw from their own generator: no lock, no shared state.
std::minstd_rand& Rng()
{
    thread_local std::minstd_rand rng(
        []
        {
            uint32 seed = uint32(std::hash<std::thread::id>()(std::this_thread::get_id()));
            try
            {
                std::random_device device;
                seed ^= device();
            }
            catch (...)
            {
            }
            return seed ? seed : 1u;
        }());
    return rng;
}

// A group drawn from a distribution sorted best first; the last group when rounding leaves a remainder.
std::size_t SampleGroup(std::vector<std::pair<std::string, float>> const& dist)
{
    float total = 0.0f;
    for (auto const& [group, p] : dist)
        total += std::max(p, 0.0f);
    if (total <= 0.0f)
        return 0;

    float const roll = std::uniform_real_distribution<float>(0.0f, total)(Rng());
    float sum = 0.0f;
    for (std::size_t i = 0; i < dist.size(); ++i)
    {
        sum += std::max(dist[i].second, 0.0f);
        if (roll < sum)
            return i;
    }
    return dist.size() - 1;
}

bool Grouped(BrainQuestion const& question)
{
    if (question.type != BrainQuestionType::Choice || question.options.empty())
        return false;
    for (BrainOption const& option : question.options)
        if (option.group.empty())
            return false;
    return true;
}

// The answer as a distribution over the options' groups, best first (ties keep the options' order).
BrainAnswer ToGroups(BrainQuestion const& question, BrainAnswer const& answer)
{
    std::vector<std::pair<std::string, float>> groups;
    auto add = [&groups](std::string const& group, float p)
    {
        for (auto& entry : groups)
        {
            if (entry.first == group)
            {
                entry.second += p;
                return;
            }
        }
        groups.emplace_back(group, p);
    };

    std::string chosenGroup;
    for (BrainOption const& option : question.options)
    {
        if (option.key == answer.choice)
            chosenGroup = option.group;
        float p = 0.0f;
        for (auto const& [key, value] : answer.dist)
        {
            if (key == option.key)
            {
                p = std::max(value, 0.0f);
                break;
            }
        }
        add(option.group, answer.dist.empty() ? 0.0f : p);
    }

    float total = 0.0f;
    for (auto const& entry : groups)
        total += entry.second;
    if (total <= 0.0f)
    {
        // No probabilities (or all zero): the chosen option's group takes everything.
        for (auto& entry : groups)
            entry.second = entry.first == chosenGroup ? 1.0f : 0.0f;
        total = chosenGroup.empty() ? 0.0f : 1.0f;
    }

    BrainAnswer stored = answer;
    stored.choice.clear();
    stored.dist.clear();
    if (total > 0.0f)
    {
        for (auto const& entry : groups)
            if (entry.second > 0.0f)
                stored.dist.emplace_back(entry.first, entry.second / total);
        std::stable_sort(stored.dist.begin(), stored.dist.end(),
                         [](auto const& a, auto const& b) { return a.second > b.second; });
    }
    stored.p = stored.dist.empty() ? 0.0f : stored.dist.front().second;
    stored.confidence = -1.0f;
    return stored;
}
}  // namespace

BrainCache& BrainCache::Instance()
{
    static BrainCache instance;
    return instance;
}

BrainCache::Shard& BrainCache::ShardFor(std::string const& key) { return shards[CoaHash::Bytes(key) % SHARDS]; }

void BrainCache::Configure(uint32 capacity, uint32 ttlSeconds)
{
    uint32 const perShard = capacity ? std::max<uint32>(1, capacity / SHARDS) : 0;
    capacityPerShard.store(perShard, std::memory_order_relaxed);
    ttlMs.store(ttlSeconds * 1000, std::memory_order_relaxed);
    if (!perShard)
    {
        Clear();
        return;
    }

    // A smaller capacity takes effect at once.
    for (Shard& shard : shards)
    {
        std::lock_guard<std::mutex> guard(shard.lock);
        while (shard.entries.size() > perShard && !shard.lru.empty())
        {
            shard.entries.erase(shard.lru.back());
            shard.lru.pop_back();
        }
    }
}

bool BrainCache::Lookup(std::string const& key, uint64 nowMs, BrainResult& out)
{
    if (!capacityPerShard.load(std::memory_order_relaxed))
        return false;

    if (Rng()() % REFRESH_ONE_IN == 0)
    {
        misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::vector<BrainAnswer> answers;
    std::vector<bool> grouped;
    std::string engine;
    std::string providerModel;
    bool calibrated = false;
    {
        Shard& shard = ShardFor(key);
        std::lock_guard<std::mutex> guard(shard.lock);
        auto const it = shard.entries.find(key);
        if (it == shard.entries.end())
        {
            misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        Entry& entry = it->second;
        if (entry.storedMs + ttlMs.load(std::memory_order_relaxed) <= nowMs)
        {
            shard.lru.erase(entry.lru);
            shard.entries.erase(it);
            misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        shard.lru.splice(shard.lru.begin(), shard.lru, entry.lru);
        answers = entry.answers;
        grouped = entry.grouped;
        engine = entry.engine;
        providerModel = entry.providerModel;
        calibrated = entry.calibrated;
    }

    // A grouped answer comes back as one group drawn from the stored distribution; the caller picks the line.
    for (std::size_t i = 0; i < answers.size(); ++i)
    {
        if (i >= grouped.size() || !grouped[i] || answers[i].dist.empty())
            continue;
        std::size_t const pick = SampleGroup(answers[i].dist);
        answers[i].choice = answers[i].dist[pick].first;
        answers[i].p = answers[i].dist[pick].second;
    }

    out.error = BrainError::None;
    out.fromCache = true;
    out.answers = std::move(answers);
    out.engine = std::move(engine);
    out.providerModel = std::move(providerModel);
    out.calibrated = calibrated;
    out.usage = BrainUsage{};
    out.httpStatus = 0;
    hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void BrainCache::Store(std::string const& key, BrainJob const& job, BrainResult const& result, uint64 nowMs)
{
    uint32 const perShard = capacityPerShard.load(std::memory_order_relaxed);
    if (!perShard || key.empty())
        return;

    Entry entry;
    for (BrainQuestion const& question : job.questions)
    {
        if (!question.cacheable)
            continue;
        BrainAnswer const* answer = result.Find(question.name);
        if (!answer || !answer->valid)
            continue;
        if (Grouped(question))
        {
            BrainAnswer stored = ToGroups(question, *answer);
            if (stored.dist.empty())
                continue;
            entry.answers.push_back(std::move(stored));
            entry.grouped.push_back(true);
        }
        else
        {
            entry.answers.push_back(*answer);
            entry.grouped.push_back(false);
        }
    }
    // Nothing reusable: an entry without answers would only turn a later call into an invalid result.
    if (entry.answers.empty())
        return;

    entry.engine = result.engine;
    entry.providerModel = result.providerModel;
    entry.calibrated = result.calibrated;
    entry.storedMs = nowMs;

    Shard& shard = ShardFor(key);
    std::lock_guard<std::mutex> guard(shard.lock);
    auto const found = shard.entries.find(key);
    if (found != shard.entries.end())
    {
        shard.lru.erase(found->second.lru);
        shard.entries.erase(found);
    }
    shard.lru.push_front(key);
    entry.lru = shard.lru.begin();
    shard.entries.emplace(key, std::move(entry));
    while (shard.entries.size() > perShard && !shard.lru.empty())
    {
        shard.entries.erase(shard.lru.back());
        shard.lru.pop_back();
    }
}

void BrainCache::Clear()
{
    for (Shard& shard : shards)
    {
        std::lock_guard<std::mutex> guard(shard.lock);
        shard.entries.clear();
        shard.lru.clear();
    }
}

uint32 BrainCache::Size() const
{
    uint32 total = 0;
    for (Shard const& shard : shards)
    {
        std::lock_guard<std::mutex> guard(shard.lock);
        total += uint32(shard.entries.size());
    }
    return total;
}
