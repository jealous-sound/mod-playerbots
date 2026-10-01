/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainRpgAdvice.h"
#include "BrainConfig.h"
#include "BrainService.h"
#include "BrainSink.h"
#include "BrainStats.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "DBCStores.h"
#include "GameEventMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "QuestionRegistry.h"
#include "RandomPlayerbotMgr.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace
{
std::mutex SnapshotMutex;
std::shared_ptr<BrainRpgSnapshot const> Snapshot = std::make_shared<BrainRpgSnapshot>();
std::atomic<float> Influence{0.0f};
constexpr std::array<char const*, BRAIN_RPG_OPTIONS> Keys{
    "wander_random", "wander_npc", "go_grind", "go_camp", "do_quest", "travel_flight", "rest", "outdoor_pvp"};
constexpr std::array<char const*, BRAIN_RPG_OPTIONS> Descriptions{
    "explore the surroundings and fight whatever they meet", "walk among the villagers and visit NPCs",
    "go to a good spot and grind monsters", "go back to camp: an inn or a hub", "work on their quests",
    "take a flight path to another area", "sit down and rest", "contest an outdoor PvP objective"};
constexpr std::array<char const*, 3> Roles{"damage dealers", "tanks", "healers"};

BrainRpgKey Unpack(uint64 packed)
{
    return {uint32(packed >> 16), uint8((packed >> 8) & 0xFF), uint8(packed & 0xFF)};
}

std::string ZoneName(uint32 zone)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(zone);
    return area && area->area_name[0] ? area->area_name[0] : "unknown zone";
}

uint32 ScaledWeight(long double value)
{
    return uint32(std::clamp(std::round(value), 1.0L,
                             static_cast<long double>(std::numeric_limits<uint32>::max())));
}

bool ReadAdvice(BrainResult const& result, BrainRpgEntry& entry)
{
    if (result.error != BrainError::None)
        return false;
    BrainAnswer const* answer = result.Find("next");
    if (!answer || !answer->valid)
        return false;
    entry.p.fill(0.0f);
    if (!answer->dist.empty())
    {
        float sum = 0.0f;
        for (auto const& [key, probability] : answer->dist)
        {
            if (!std::isfinite(probability) || probability < 0.0f)
                continue;
            auto found = std::find_if(Keys.begin(), Keys.end(), [&](char const* value) { return key == value; });
            if (found == Keys.end())
                continue;
            entry.p[std::size_t(found - Keys.begin())] = probability;
        }
        for (float probability : entry.p)
            sum += probability;
        if (!(sum > 0.0f) || !std::isfinite(sum))
            return false;
        for (float& probability : entry.p)
            probability /= sum;
        return true;
    }
    auto found = std::find_if(Keys.begin(), Keys.end(), [&](char const* value) { return answer->choice == value; });
    if (found == Keys.end() || !std::isfinite(answer->p))
        return false;
    float const chosen = std::clamp(answer->p, 0.125f, 1.0f);
    entry.p.fill((1.0f - chosen) / 7.0f);
    entry.p[std::size_t(found - Keys.begin())] = chosen;
    return true;
}

BrainJob BuildJob(BrainRpgKey const& key, uint32 bots, uint32 humans, uint64 now, uint32 epoch)
{
    BrainJob job;
    job.use = BrainUse::Rpg;
    job.lane = BrainLane::Ambient;
    job.situation = "rpg_next";
    job.tag = key.Packed();
    job.epoch = epoch;
    job.deadlineMs = now + 8000;
    BrainQuestion question;
    question.name = "next";
    question.cacheable = false;
    question.instructions = "Which activity should this group of adventurers do next? Consider how many real "
        "players are around (lively places are more fun), the time of day and the active events. "
        "Answer with the option that fits best.";
    for (std::size_t i = 0; i < Keys.size(); ++i)
        question.options.push_back({Keys[i], Descriptions[i], {}});
    job.questions.push_back(std::move(question));
    std::time_t const unixTime = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &unixTime);
#else
    localtime_r(&unixTime, &local);
#endif
    char const* part = local.tm_hour < 6 ? "night" : local.tm_hour < 12 ? "morning"
                          : local.tm_hour < 18 ? "afternoon" : "evening";
    std::string events;
    auto const& eventMap = sGameEventMgr->GetEventMap();
    uint32 eventCount = 0;
    for (uint16 id : sGameEventMgr->GetActiveEventList())
    {
        if (id >= eventMap.size() || eventMap[id].Description.empty())
            continue;
        if (!events.empty())
            events += ", ";
        events += eventMap[id].Description;
        if (++eventCount == 3)
            break;
    }
    std::ostringstream state;
    state << "[realm]\n" << QuestionRegistry::Current()->header << "\n[situation]\nplace: " << ZoneName(key.zoneId)
          << "\nlevel band: " << uint32(key.band) * 10 << '-' << uint32(key.band) * 10 + 9
          << "\nrole: " << Roles[std::min<std::size_t>(key.role, 2)]
          << "\nadventurers in this group: " << bots << "\nreal players in this zone: " << humans
          << "\ntime of day: " << local.tm_hour << ":00 (" << part << ")\nactive events: "
          << (events.empty() ? "none" : events) << '\n';
    job.state = state.str();
    return job;
}
}

struct BrainRpgAdvice::Impl
{
    struct Pending
    {
        uint64 key = 0;
        uint32 bots = 0;
        uint64 deadline = 0;
        std::shared_ptr<IBrain> brain;
    };
    std::shared_ptr<BrainSink> sink = std::make_shared<BrainSink>(128);
    std::unordered_map<uint64, Pending> pending;
    std::unordered_map<uint64, uint64> failedUntil;
    uint64 nextTick = 0;
    uint32 epoch = 1;
};

char const* BrainRpgOptionKey(BrainRpgOption option)
{
    std::size_t const index = std::size_t(option);
    return index < Keys.size() ? Keys[index] : "";
}

BrainRpgAdvice& BrainRpgAdvice::Instance()
{
    static BrainRpgAdvice instance;
    return instance;
}

std::shared_ptr<BrainRpgSnapshot const> BrainRpgAdvice::Current()
{
    std::lock_guard<std::mutex> lock(SnapshotMutex);
    return Snapshot;
}

int32 BrainRpgAdvice::OptionOfStatus(int32 status)
{
    switch (status)
    {
        case 3: return int32(BrainRpgOption::WanderRandom);
        case 4: return int32(BrainRpgOption::WanderNpc);
        case 1: return int32(BrainRpgOption::GoGrind);
        case 2: return int32(BrainRpgOption::GoCamp);
        case 5: return int32(BrainRpgOption::DoQuest);
        case 6: return int32(BrainRpgOption::TravelFlight);
        case 7: return int32(BrainRpgOption::Rest);
        case 8: return int32(BrainRpgOption::OutdoorPvp);
        default: return -1;
    }
}

BrainRpgKey BrainRpgAdvice::KeyOf(Player* bot)
{
    if (!bot)
        return {};
    uint8 const role = PlayerbotAI::IsTank(bot) ? 1 : PlayerbotAI::IsHeal(bot) ? 2 : 0;
    return {bot->GetZoneId(), uint8(std::min<uint32>(bot->GetLevel() / 10, 6)), role};
}

bool BrainRpgAdvice::BlendWeights(BrainRpgEntry const& entry, float influence, int32 const* options,
                                  uint32* weights, std::size_t count)
{
    if (!options || !weights || !std::isfinite(influence) || influence <= 0.0f)
        return false;
    std::size_t eligible = 0;
    long double sum = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!weights[i] || options[i] < 0 || std::size_t(options[i]) >= BRAIN_RPG_OPTIONS)
            continue;
        float const probability = entry.p[options[i]];
        if (!std::isfinite(probability) || probability < 0.0f)
            continue;
        ++eligible;
        sum += probability;
    }
    if (eligible < 2 || !(sum > 0))
        return false;
    long double const amount = std::min(influence, 1.0f);
    uint64 total = 0;
    std::vector<uint32> changed(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        long double factor = 1;
        if (weights[i] && options[i] >= 0 && std::size_t(options[i]) < BRAIN_RPG_OPTIONS &&
            std::isfinite(entry.p[options[i]]) && entry.p[options[i]] >= 0.0f)
            factor = 1 - amount + amount * eligible * entry.p[options[i]] / sum;
        changed[i] = weights[i] ? ScaledWeight(16.0L * weights[i] * factor) : 0;
        total += changed[i];
    }
    if (total > std::numeric_limits<uint32>::max())
        return false;
    std::copy(changed.begin(), changed.end(), weights);
    return true;
}

bool BrainRpgAdvice::Blend(Player* bot, int32 const* statuses, uint32* weights, std::size_t count)
{
    if (!Active() || !bot || !statuses || !weights)
        return false;
    auto snapshot = Current();
    BrainRpgEntry const* entry = snapshot->Find(KeyOf(bot));
    if (!entry || entry->untilMs <= CoaTime::NowMs())
        return false;
    std::vector<int32> options(count);
    for (std::size_t i = 0; i < count; ++i)
        options[i] = OptionOfStatus(statuses[i]);
    if (!BlendWeights(*entry, Influence.load(std::memory_order_relaxed), options.data(), weights, count))
        return false;
    BrainStats::Instance().rpgApplied.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void BrainRpgAdvice::Update(uint64 nowMs)
{
    COA_ASSERT_WORLD_THREAD();
    auto config = BrainConfig::Current();
    auto brain = BrainService::Current();
    if (!config->enable || !config->useRpgNext || !brain->Active(BrainUse::Rpg))
    {
        if (impl || Active())
            Clear();
        return;
    }
    if (!impl)
        impl = std::make_shared<Impl>();
    if (nowMs < impl->nextTick)
        return;
    impl->nextTick = nowMs + 10000;
    Influence.store(config->rpgInfluence, std::memory_order_relaxed);
    auto previous = Current();
    auto next = std::make_shared<BrainRpgSnapshot>();
    next->version = previous->version + 1;
    for (auto const& [key, entry] : previous->entries)
        if (entry.untilMs > nowMs)
            next->entries.emplace(key, entry);
    std::vector<BrainResult> results;
    impl->sink->Drain(results);
    for (BrainResult const& result : results)
    {
        auto waiting = impl->pending.find(result.jobId);
        if (waiting == impl->pending.end() || result.epoch != impl->epoch || result.tag != waiting->second.key)
            continue;
        Impl::Pending const pending = waiting->second;
        impl->pending.erase(waiting);
        BrainRpgEntry entry;
        if (ReadAdvice(result, entry))
        {
            entry.bots = pending.bots;
            entry.untilMs = nowMs + uint64(config->rpgAdviceTtlSeconds) * 1000;
            next->entries[pending.key] = entry;
            BrainStats::Instance().rpgAnswered.fetch_add(1, std::memory_order_relaxed);
        }
        else
            impl->failedUntil[pending.key] = nowMs + 60000;
    }
    for (auto it = impl->pending.begin(); it != impl->pending.end();)
    {
        if (it->second.deadline < nowMs)
        {
            it->second.brain->Cancel(it->first);
            impl->failedUntil[it->second.key] = nowMs + 60000;
            it = impl->pending.erase(it);
        }
        else
            ++it;
    }
    for (auto it = impl->failedUntil.begin(); it != impl->failedUntil.end();)
        if (it->second <= nowMs)
            it = impl->failedUntil.erase(it);
        else
            ++it;
    {
        std::lock_guard<std::mutex> lock(SnapshotMutex);
        Snapshot = next;
    }
    active.store(!next->entries.empty(), std::memory_order_relaxed);
    BrainStats::Instance().rpgEntries.store(next->entries.size(), std::memory_order_relaxed);
    std::unordered_map<uint64, uint32> counts;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!bot || !bot->IsInWorld() || bot->IsInCombat() || bot->InBattleground())
            continue;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (ai && ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
            ++counts[KeyOf(bot).Packed()];
    }
    std::unordered_map<uint32, uint32> humans;
    for (auto const& [account, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = session ? session->GetPlayer() : nullptr;
        if (player && player->IsInWorld() && !GET_PLAYERBOT_AI(player))
            ++humans[player->GetZoneId()];
    }
    std::vector<std::pair<uint64, uint32>> candidates;
    for (auto const& [key, bots] : counts)
    {
        bool const waiting = std::any_of(impl->pending.begin(), impl->pending.end(),
            [&](auto const& job) { return job.second.key == key; });
        if (bots >= config->rpgMinBots && !next->entries.count(key) && !waiting && !impl->failedUntil.count(key))
            candidates.emplace_back(key, bots);
    }
    std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b)
    {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    uint32 submitted = 0;
    for (auto const& [key, bots] : candidates)
    {
        if (submitted == 2 || impl->pending.size() >= 24)
            break;
        BrainRpgKey const unpacked = Unpack(key);
        BrainJob job = BuildJob(unpacked, bots, humans[unpacked.zoneId], nowMs, impl->epoch);
        ++submitted;
        BrainTicket const ticket = brain->Submit(std::move(job), impl->sink, nullptr);
        if (ticket.status == BrainSubmit::Queued)
        {
            impl->pending.emplace(ticket.jobId, Impl::Pending{key, bots, nowMs + 8000, brain});
            BrainStats::Instance().rpgAsked.fetch_add(1, std::memory_order_relaxed);
        }
        else
            impl->failedUntil[key] = nowMs + 60000;
    }
}

void BrainRpgAdvice::OnConfigReload()
{
    COA_ASSERT_WORLD_THREAD();
    if (impl || Active())
        Clear();
}

void BrainRpgAdvice::Shutdown()
{
    COA_ASSERT_WORLD_THREAD();
    if (impl || Active())
        Clear();
}

std::vector<std::string> BrainRpgAdvice::Clear()
{
    COA_ASSERT_WORLD_THREAD();
    active.store(false, std::memory_order_relaxed);
    if (impl)
    {
        for (auto const& [id, pending] : impl->pending)
            pending.brain->Cancel(id);
        impl->sink->Close();
        impl.reset();
    }
    auto empty = std::make_shared<BrainRpgSnapshot>();
    {
        std::lock_guard<std::mutex> lock(SnapshotMutex);
        empty->version = Snapshot->version + 1;
        Snapshot = std::move(empty);
    }
    BrainStats::Instance().rpgEntries.store(0, std::memory_order_relaxed);
    return {"rpg advice cleared"};
}

std::vector<std::string> BrainRpgAdvice::FormatStatus() const
{
    COA_ASSERT_WORLD_THREAD();
    auto config = BrainConfig::Current();
    BrainStats const& stats = BrainStats::Instance();
    std::ostringstream header;
    header << "rpg advice: " << (config->enable && config->useRpgNext ? "on" : "off")
           << " (Use.RpgNext), influence " << std::fixed << std::setprecision(2) << config->rpgInfluence
           << ", ttl " << config->rpgAdviceTtlSeconds << " s, min bots " << config->rpgMinBots
           << "; asked/answered/applied/entries " << stats.rpgAsked.load(std::memory_order_relaxed) << '/'
           << stats.rpgAnswered.load(std::memory_order_relaxed) << '/'
           << stats.rpgApplied.load(std::memory_order_relaxed) << '/'
           << stats.rpgEntries.load(std::memory_order_relaxed);
    std::vector<std::string> lines{header.str()};
    uint64 const now = CoaTime::NowMs();
    auto snapshot = Current();
    for (auto const& [key, entry] : snapshot->entries)
    {
        BrainRpgKey const group = Unpack(key);
        std::ostringstream line;
        line << ZoneName(group.zoneId) << " band " << uint32(group.band) * 10 << '-'
             << uint32(group.band) * 10 + 9 << ' ' << Roles[std::min<std::size_t>(group.role, 2)]
             << ": " << entry.bots << " bots, expires in "
             << (entry.untilMs > now ? (entry.untilMs - now) / 1000 : 0) << " s:";
        std::array<std::size_t, BRAIN_RPG_OPTIONS> order{0, 1, 2, 3, 4, 5, 6, 7};
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return entry.p[a] > entry.p[b]; });
        for (std::size_t i = 0; i < 5; ++i)
            line << ' ' << Keys[order[i]] << ' ' << std::fixed << std::setprecision(2) << entry.p[order[i]];
        lines.push_back(line.str());
    }
    return lines;
}
