/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityDuelCooldowns.h"
#include "ActivityBrain.h"
#include "ActivityTypes.h"
#include "BrainSink.h"
#include "BrainTypes.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldSession.h"
#include <algorithm>
#include <cstddef>
#include <utility>

namespace
{
constexpr std::size_t RESULTS_MAX = 256;
constexpr uint64 OUTCOME_KEEP_MS = 60 * 1000;
constexpr uint64 DECISION_GRACE_MS = 10 * 1000;
constexpr uint32 SINK_CAPACITY = 256;

bool IsBotSession(Player* player) { return player->GetSession() && player->GetSession()->IsBot(); }

uint32 AccountOf(Player* player) { return player->GetSession() ? player->GetSession()->GetAccountId() : 0; }

template <typename Map>
void EraseUntilPast(Map& map, uint64 nowMs)
{
    for (auto it = map.begin(); it != map.end();)
    {
        if (it->second <= nowMs)
            it = map.erase(it);
        else
            ++it;
    }
}
}  // namespace

ActivityDuelCooldowns& ActivityDuelCooldowns::Instance()
{
    static ActivityDuelCooldowns instance;
    return instance;
}

ActivityDuelCooldowns::ActivityDuelCooldowns() : sink(std::make_shared<BrainSink>(SINK_CAPACITY)) {}

uint64 ActivityDuelCooldowns::PairKey(uint32 lowA, uint32 lowB)
{
    return (uint64(std::min(lowA, lowB)) << 32) | uint64(std::max(lowA, lowB));
}

bool ActivityDuelCooldowns::PairReady(uint32 lowA, uint32 lowB, uint64 nowMs) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto const it = pairUntilMs.find(PairKey(lowA, lowB));
    return it == pairUntilMs.end() || it->second <= nowMs;
}

bool ActivityDuelCooldowns::PlayerReady(uint32 account, uint64 nowMs) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto const cooldown = playerUntilMs.find(account);
    if (cooldown != playerUntilMs.end() && cooldown->second > nowMs)
        return false;
    auto const declined = declinedUntilMs.find(account);
    return declined == declinedUntilMs.end() || declined->second <= nowMs;
}

uint32 ActivityDuelCooldowns::RealDuelsToday(uint32 account, uint32 utcDay) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto const it = realDuelsByAccount.find(account);
    return it != realDuelsByAccount.end() && it->second.day == utcDay ? it->second.count : 0;
}

uint64 ActivityDuelCooldowns::LastOwnDuelEndMs(uint32 botLow) const
{
    std::lock_guard<std::mutex> guard(lock);
    auto const it = lastOwnDuelEndMs.find(botLow);
    return it == lastOwnDuelEndMs.end() ? 0 : it->second;
}

bool ActivityDuelCooldowns::TryStampChallenge(uint32 botLow, uint32 targetLow, uint32 targetAccount, uint64 nowMs,
                                              uint32 pairCooldownMs, uint32 playerCooldownMs)
{
    std::lock_guard<std::mutex> guard(lock);
    uint64 const key = PairKey(botLow, targetLow);
    auto const pair = pairUntilMs.find(key);
    if (pair != pairUntilMs.end() && pair->second > nowMs)
        return false;
    if (targetAccount)
    {
        auto const cooldown = playerUntilMs.find(targetAccount);
        if (cooldown != playerUntilMs.end() && cooldown->second > nowMs)
            return false;
        auto const declined = declinedUntilMs.find(targetAccount);
        if (declined != declinedUntilMs.end() && declined->second > nowMs)
            return false;
    }

    pairUntilMs[key] = nowMs + pairCooldownMs;
    if (targetAccount)
        playerUntilMs[targetAccount] = nowMs + playerCooldownMs;
    challenges.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ActivityDuelCooldowns::OnDuelEnd(Player* winner, Player* loser, uint8 type)
{
    if (!winner || !loser || !winner->duel || !loser->duel)
        return;

    uint64 const nowMs = CoaTime::NowMs();
    bool const started = loser->duel->StartTime != 0;
    bool const winnerBot = IsBotSession(winner);
    bool const loserBot = IsBotSession(loser);

    // Declined before it began: the loser is the challenged one who said no to the winner's challenge.
    if (type == DUEL_INTERRUPTED && !started && loser != loser->duel->Initiator)
    {
        if (winnerBot && !loserBot)
        {
            uint32 const account = AccountOf(loser);
            if (!account)
                return;
            std::lock_guard<std::mutex> guard(lock);
            declinedUntilMs[account] = nowMs + ActivityThreshold::DECLINE_COOLDOWN_MS;
            declines.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    if (!started || (type != DUEL_WON && type != DUEL_FLED) || (!winnerBot && !loserBot))
        return;

    bool const fled = type == DUEL_FLED;
    uint32 const realAccount = winnerBot == loserBot ? 0 : AccountOf(winnerBot ? loser : winner);
    uint32 const today = realAccount ? BrainClock::UtcDayNow() : 0;

    std::lock_guard<std::mutex> guard(lock);
    if (winnerBot)
    {
        uint32 const low = winner->GetGUID().GetCounter();
        outcomes[low] = {ActivityDuelOutcomeKind::Won, loser->GetGUID(), loserBot, nowMs};
        lastOwnDuelEndMs[low] = nowMs;
    }
    if (loserBot)
    {
        uint32 const low = loser->GetGUID().GetCounter();
        outcomes[low] = {fled ? ActivityDuelOutcomeKind::Fled : ActivityDuelOutcomeKind::Lost, winner->GetGUID(),
                         winnerBot, nowMs};
        lastOwnDuelEndMs[low] = nowMs;
    }

    if (results.size() >= RESULTS_MAX)
        results.erase(results.begin(), results.begin() + std::ptrdiff_t(results.size() - RESULTS_MAX + 1));
    results.push_back({winner->GetGUID(), loser->GetGUID(), winnerBot, loserBot, fled, nowMs});

    if (realAccount)
    {
        DayCount& count = realDuelsByAccount[realAccount];
        if (count.day != today)
            count = {today, 0};
        ++count.count;
        realDuels.fetch_add(1, std::memory_order_relaxed);
    }
}

std::optional<ActivityDuelOutcome> ActivityDuelCooldowns::TakeOutcome(uint32 botLow)
{
    std::lock_guard<std::mutex> guard(lock);
    auto const it = outcomes.find(botLow);
    if (it == outcomes.end())
        return std::nullopt;
    ActivityDuelOutcome const outcome = it->second;
    outcomes.erase(it);
    return outcome;
}

void ActivityDuelCooldowns::BeginDecision(uint32 botLow, ActivityDuelPending const& decision)
{
    std::lock_guard<std::mutex> guard(lock);
    auto const old = pending.find(botLow);
    if (old != pending.end())
        pendingByTag.erase(old->second.tag);
    pending[botLow] = decision;
    pendingByTag[decision.tag] = botLow;
    brainAsked.fetch_add(1, std::memory_order_relaxed);
}

std::optional<ActivityDuelPending> ActivityDuelCooldowns::TakeDecision(uint32 botLow, uint64 epoch, uint64 nowMs)
{
    std::lock_guard<std::mutex> guard(lock);
    auto const it = pending.find(botLow);
    if (it == pending.end())
        return std::nullopt;

    if (it->second.epoch != epoch)
    {
        pendingByTag.erase(it->second.tag);
        pending.erase(it);
        return std::nullopt;
    }

    ActivityDuelPending decision = it->second;
    if (decision.verdict == ActivityDuelVerdict::Waiting && nowMs < decision.deadlineMs)
        return decision;

    if (decision.verdict == ActivityDuelVerdict::Waiting)
        decision.verdict = ActivityDuelVerdict::Fallback;
    pendingByTag.erase(decision.tag);
    pending.erase(it);
    return decision;
}

void ActivityDuelCooldowns::DrainBrain(float acceptThreshold)
{
    COA_ASSERT_WORLD_THREAD();
    if (sink->Empty())
        return;

    std::vector<BrainResult> drained;
    sink->Drain(drained);

    // The answers are read outside the lock (pure), then applied.
    std::vector<std::pair<uint64, ActivityDuelVerdict>> verdicts;
    verdicts.reserve(drained.size());
    for (BrainResult const& result : drained)
    {
        ActivityDuelVerdict verdict = ActivityDuelVerdict::Fallback;
        if (result.error == BrainError::None)
            if (std::optional<bool> const accept = ActivityBrain::ReadDuel(result, acceptThreshold))
                verdict = *accept ? ActivityDuelVerdict::Yes : ActivityDuelVerdict::No;
        verdicts.emplace_back(result.tag, verdict);
    }

    std::lock_guard<std::mutex> guard(lock);
    for (auto const& [tag, verdict] : verdicts)
    {
        auto const byTag = pendingByTag.find(tag);
        if (byTag == pendingByTag.end())
            continue;
        auto const entry = pending.find(byTag->second);
        pendingByTag.erase(byTag);
        if (entry == pending.end() || entry->second.tag != tag ||
            entry->second.verdict != ActivityDuelVerdict::Waiting)
            continue;
        entry->second.verdict = verdict;
        LOG_DEBUG("playerbots.activity", "duel accept of bot {}: brain says {}", entry->first,
                  verdict == ActivityDuelVerdict::Yes ? "yes" : verdict == ActivityDuelVerdict::No ? "no" : "nothing");
    }
}

void ActivityDuelCooldowns::DrainResults(std::vector<ActivityDuelResult>& out)
{
    std::lock_guard<std::mutex> guard(lock);
    if (results.empty())
        return;
    out.insert(out.end(), std::make_move_iterator(results.begin()), std::make_move_iterator(results.end()));
    results.clear();
}

void ActivityDuelCooldowns::Prune(uint64 nowMs, uint32 utcDay)
{
    std::lock_guard<std::mutex> guard(lock);
    EraseUntilPast(pairUntilMs, nowMs);
    EraseUntilPast(playerUntilMs, nowMs);
    EraseUntilPast(declinedUntilMs, nowMs);

    for (auto it = realDuelsByAccount.begin(); it != realDuelsByAccount.end();)
    {
        if (it->second.day != utcDay)
            it = realDuelsByAccount.erase(it);
        else
            ++it;
    }
    for (auto it = outcomes.begin(); it != outcomes.end();)
    {
        if (nowMs >= it->second.endMs + OUTCOME_KEEP_MS)
            it = outcomes.erase(it);
        else
            ++it;
    }
    // Past the rest time a bot's last duel no longer matters (0 reads as "rested").
    for (auto it = lastOwnDuelEndMs.begin(); it != lastOwnDuelEndMs.end();)
    {
        if (nowMs >= it->second + ActivityThreshold::BOT_DUEL_REST_MS)
            it = lastOwnDuelEndMs.erase(it);
        else
            ++it;
    }
    for (auto it = pending.begin(); it != pending.end();)
    {
        if (nowMs >= it->second.deadlineMs + DECISION_GRACE_MS)
        {
            pendingByTag.erase(it->second.tag);
            it = pending.erase(it);
        }
        else
            ++it;
    }
}

void ActivityDuelCooldowns::Clear()
{
    std::lock_guard<std::mutex> guard(lock);
    pairUntilMs.clear();
    playerUntilMs.clear();
    declinedUntilMs.clear();
    realDuelsByAccount.clear();
    lastOwnDuelEndMs.clear();
    outcomes.clear();
    results.clear();
    pending.clear();
    pendingByTag.clear();
}

void ActivityDuelCooldowns::Shutdown()
{
    Clear();
    sink->Close();
}
