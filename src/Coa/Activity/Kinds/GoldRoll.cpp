/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GoldRoll.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "ActivityPvePolicy.h"
#include "CoaActivityValues.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "StringFormat.h"
#include "TradeData.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <mutex>
#include <unordered_map>

namespace
{
constexpr float NEAR_YARDS = 10.0f;
constexpr uint64 CONSENT_MS = 60000;
constexpr uint64 SETTLE_MS = 120000;
constexpr uint32 MAX_TASKS = 64;

enum class Outcome : uint8 { Waiting, Settled, Cancelled };
struct RollTask
{
    uint32 activity = 0;
    uint64 epoch = 0;
    ObjectGuid requester;
    uint32 stake = 0;
    uint32 botRoll = 0;
    uint32 playerRoll = 0;
    uint64 untilMs = 0;
    bool accepted = false;
    bool announced = false;
    bool initiated = false;
    bool cancelled = false;
    Outcome outcome = Outcome::Waiting;
};

struct RollState : ActivityKindState
{
    RollTask task;
    ObjectGuid bot;
    uint32 map = 0;
    std::string line;
    uint64 lineUntilMs = 0;
    bool termsDelivered = false;
};

std::mutex rollMutex;
std::unordered_map<uint32, RollTask> rolls;
std::atomic<bool> anyRolls{false};

bool Read(uint32 low, RollTask& task)
{
    if (!anyRolls.load(std::memory_order_relaxed))
        return false;
    std::lock_guard<std::mutex> lock(rollMutex);
    auto const found = rolls.find(low);
    if (found == rolls.end())
        return false;
    task = found->second;
    return true;
}

void Write(uint32 low, RollTask const& task)
{
    std::lock_guard<std::mutex> lock(rollMutex);
    auto const found = rolls.find(low);
    if (found != rolls.end() && found->second.activity == task.activity && found->second.epoch == task.epoch &&
        !found->second.cancelled)
        found->second = task;
}

bool Install(uint32 low, RollTask const& task, uint64 now)
{
    std::lock_guard<std::mutex> lock(rollMutex);
    for (auto it = rolls.begin(); it != rolls.end();)
        if (it->second.untilMs + 60000 < now)
            it = rolls.erase(it);
        else
            ++it;
    if (rolls.size() >= MAX_TASKS && !rolls.count(low))
        return false;
    rolls[low] = task;
    anyRolls.store(true, std::memory_order_relaxed);
    return true;
}

bool Human(Player* player)
{
    return ActivityPvePolicy::SafePlayer(player) && !player->IsInCombat() && player->GetSession() &&
           !player->GetSession()->IsBot() && !player->GetSession()->isLogingOut() &&
           !player->isAFK() && !player->isDND() && !player->GetGroup() && !player->GetGroupInvite();
}

bool Pair(Player* bot, Player* player, uint64 epoch)
{
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    return ai && ai->GetCoaEpoch() == epoch && ActivityPvePolicy::SafePlayer(bot) && !bot->IsInCombat() &&
           Human(player) && bot->GetMap() == player->GetMap() && bot->InSamePhase(player) &&
           bot->GetTeamId() == player->GetTeamId() && bot->GetGUID().GetRealmID() == player->GetGUID().GetRealmID() &&
           bot->GetExactDist(player) <= NEAR_YARDS &&
           !bot->GetGroup() && !ai->GetMaster();
}

bool Enough(Player* bot, Player* player, uint32 stake)
{
    return stake && bot->GetMoney() >= stake && player->GetMoney() >= stake &&
           uint64(bot->GetMoney()) + stake < uint64(MAX_MONEY_AMOUNT) &&
           uint64(player->GetMoney()) + stake < uint64(MAX_MONEY_AMOUNT) &&
           !bot->GetSession()->IsTrialAccount() && !player->GetSession()->IsTrialAccount();
}

bool AnyItem(TradeData* trade)
{
    for (uint32 slot = 0; slot < TRADE_SLOT_COUNT; ++slot)
        if (trade->GetItem(TradeSlots(slot)))
            return true;
    return false;
}

void CancelOwn(PlayerbotAI* ai, RollTask const& task)
{
    if (!task.initiated || !ai->GetBot()->GetSession() || !ai->GetBot()->GetTrader() ||
        ai->GetBot()->GetTrader()->GetGUID() != task.requester)
        return;
    WorldPacket packet(CMSG_CANCEL_TRADE);
    ai->GetBot()->GetSession()->HandleCancelTradeOpcode(packet);
}

void Post(ActivityServices& services, Activity& activity, RollState& state)
{
    if (state.line.empty())
        return;
    if (services.nowMs >= state.lineUntilMs)
    {
        state.line.clear();
        activity.Finish(ActivityOutcome::Timeout, "gold-roll terms/result announcement could not be delivered");
        return;
    }
    auto const result = services.ops.PostToChannel(state.bot, ActivityChatTarget::Whisper,
                                                  state.line, activity.requester);
    if (result == ActivityOpResult::Done)
    {
        state.line.clear();
        if (!state.task.accepted)
            state.termsDelivered = true;
        if (state.task.accepted)
        {
            RollTask task;
            if (Read(state.bot.GetCounter(), task))
            {
                task.announced = true;
                Write(state.bot.GetCounter(), task);
            }
        }
    }
    else if (result != ActivityOpResult::Deferred && result != ActivityOpResult::Refused)
        activity.Finish(ActivityOutcome::Stopped, "gold-roll requester could not receive terms/result");
}
}

bool GoldRollRules::ParseTarget(std::string const& target, uint32& botLow, uint32& stake)
{
    botLow = 0;
    stake = 0;
    std::size_t const colon = target.find(':');
    if (colon == std::string::npos || !colon || colon + 1 == target.size())
        return false;
    auto number = [](char const* first, char const* last, uint32& value)
    {
        if (*first < '0' || *first > '9')
            return false;
        auto const result = std::from_chars(first, last, value);
        return result.ec == std::errc() && result.ptr == last && value;
    };
    char const* const start = target.data();
    return number(start, start + colon, botLow) && number(start + colon + 1, start + target.size(), stake);
}

bool GoldRollRules::SelfTest()
{
    uint32 bot = 0;
    uint32 stake = 0;
    if (!ParseTarget("12:100", bot, stake) || bot != 12 || stake != 100 ||
        Winner(1, 100) != -1 || Winner(100, 1) != 1 || Winner(50, 50) != 0 ||
        !ExactOffer(100, true, 0, 100, false, false) || !ExactOffer(100, false, 100, 0, false, false) ||
        ExactOffer(100, true, 1, 100, false, false) || ExactOffer(100, true, 0, 99, false, false) ||
        ExactOffer(100, true, 0, 101, false, false) || ExactOffer(100, true, 0, 100, true, false) ||
        ExactOffer(100, true, 0, 100, false, true) || ExactOffer(0, true, 0, 0, false, false) ||
        !Settled(100, true, 1000, 1000, 1100, 900) || !Settled(100, false, 1000, 1000, 900, 1100) ||
        Settled(100, true, 1000, 1000, 1100, 1000) || Settled(100, true, 1000, 99, 1100, 0) ||
        Settled(100, true, 0xffffffff, 1000, 99, 900))
        return false;
    for (char const* invalid : {"", "0:1", "1:0", "1", "1:", ":1", "1:2:3", "-1:1", "+1:1",
                               "1:4294967296", "1: 1", "1:1 "})
        if (ParseTarget(invalid, bot, stake))
            return false;
    return true;
}

bool GoldRollActions::Active(PlayerbotAI* ai)
{
    if (!ai)
        return false;
    ActivityMemberView const* const row = CoaActivityBot::Refresh(ai).row;
    return row && row->kind == ActivityKindId::GoldRoll;
}

void GoldRollActions::Release(PlayerbotAI* ai)
{
    if (!ai)
        return;
    uint32 const low = ai->GetBot()->GetGUID().GetCounter();
    RollTask task;
    if (!Read(low, task) || task.epoch != ai->GetCoaEpoch())
        return;
    CancelOwn(ai, task);
    std::lock_guard<std::mutex> lock(rollMutex);
    auto const found = rolls.find(low);
    if (found != rolls.end() && found->second.activity == task.activity && found->second.epoch == task.epoch)
        rolls.erase(found);
    anyRolls.store(!rolls.empty(), std::memory_order_relaxed);
}

void GoldRollActions::Tick(PlayerbotAI* ai)
{
    if (!ai)
        return;
    Player* const bot = ai->GetBot();
    RollTask task;
    if (!Read(bot->GetGUID().GetCounter(), task) || task.epoch != ai->GetCoaEpoch())
        return;
    ActivityMemberView const* const row = CoaActivityBot::Refresh(ai).row;
    if (task.cancelled || !row || row->kind != ActivityKindId::GoldRoll || row->activityId != task.activity)
    {
        Release(ai);
        return;
    }
    if (!task.accepted || !task.announced)
    {
        // Held for an explicit proposal: unsolicited trades never reach generic automatic acceptance.
        if (bot->GetTradeData())
        {
            WorldPacket packet(CMSG_CANCEL_TRADE);
            bot->GetSession()->HandleCancelTradeOpcode(packet);
        }
        return;
    }
    if (task.outcome != Outcome::Waiting)
        return;
    Player* const player = ObjectAccessor::GetPlayer(*bot, task.requester);
    if (!Pair(bot, player, task.epoch) || CoaTime::NowMs() >= task.untilMs || !Enough(bot, player, task.stake))
    {
        CancelOwn(ai, task);
        task.outcome = Outcome::Cancelled;
        Write(bot->GetGUID().GetCounter(), task);
        return;
    }
    if (task.botRoll == task.playerRoll)
    {
        task.outcome = Outcome::Settled;  // Tie: no money is offered or transferred.
        Write(bot->GetGUID().GetCounter(), task);
        return;
    }
    if (!task.initiated)
    {
        if (bot->GetTradeData() || player->GetTradeData())
        {
            // The consenting human may open this same native trade first. Anyone else is refused.
            if (bot->GetTrader() == player && player->GetTrader() == bot)
                task.initiated = true;
            else
            {
                if (bot->GetTradeData())
                {
                    WorldPacket cancel(CMSG_CANCEL_TRADE);
                    bot->GetSession()->HandleCancelTradeOpcode(cancel);
                }
                task.outcome = Outcome::Cancelled;
                Write(bot->GetGUID().GetCounter(), task);
                return;
            }
        }
        else
        {
            WorldPacket packet(CMSG_INITIATE_TRADE);
            packet << task.requester;
            bot->GetSession()->HandleInitiateTradeOpcode(packet);
        }
        if (bot->GetTrader() == player && player->GetTrader() == bot)
        {
            task.initiated = true;
            WorldPacket begin(CMSG_BEGIN_TRADE);
            bot->GetSession()->HandleBeginTradeOpcode(begin);
        }
        else
            task.outcome = Outcome::Cancelled;
        Write(bot->GetGUID().GetCounter(), task);
        return;
    }
    TradeData* const ours = bot->GetTradeData();
    TradeData* const theirs = player->GetTradeData();
    if (!ours || !theirs || bot->GetTrader() != player || player->GetTrader() != bot)
    {
        task.outcome = Outcome::Cancelled;
        Write(bot->GetGUID().GetCounter(), task);
        return;
    }
    bool const botWins = task.botRoll > task.playerRoll;
    if (AnyItem(ours) || AnyItem(theirs) || ours->GetSpell() || theirs->GetSpell())
    {
        CancelOwn(ai, task);
        task.outcome = Outcome::Cancelled;
        Write(bot->GetGUID().GetCounter(), task);
        return;
    }
    uint32 const expected = botWins ? 0 : task.stake;
    if (ours->GetMoney() != expected)
    {
        WorldPacket packet(CMSG_SET_TRADE_GOLD);
        packet << expected;
        bot->GetSession()->HandleSetTradeGoldOpcode(packet);
        return;  // Native offer mutation clears accept flags. Human must accept the exact fresh offer.
    }
    if (!theirs->IsAccepted())
        return;
    if (!GoldRollRules::ExactOffer(task.stake, botWins, ours->GetMoney(), theirs->GetMoney(), false, false))
    {
        CancelOwn(ai, task);
        task.outcome = Outcome::Cancelled;
        Write(bot->GetGUID().GetCounter(), task);
        return;
    }
    uint32 const beforeBot = bot->GetMoney();
    uint32 const beforePlayer = player->GetMoney();
    WorldPacket packet(CMSG_ACCEPT_TRADE);
    packet << uint32(0);
    bot->GetSession()->HandleAcceptTradeOpcode(packet);
    // Native handler executes both money mutations and ordinary same-realm transaction. Observe completion
    // immediately, before another command could change a balance. Never issue another accept after an ambiguous result.
    bool const complete = !bot->GetTradeData() && !player->GetTradeData();
    task.outcome = complete && GoldRollRules::Settled(task.stake, botWins, beforeBot, beforePlayer,
                                                     bot->GetMoney(), player->GetMoney()) ?
                   Outcome::Settled : Outcome::Cancelled;
    if (!complete)
        CancelOwn(ai, task);
    Write(bot->GetGUID().GetCounter(), task);
}

bool GoldRollKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                        std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    uint32 low = 0;
    uint32 stake = 0;
    Player* const player = request.nearPlayer;
    if (!services.config.goldRollEnable || !services.config.goldRollMax || !request.capacity)
        why = "gold rolls disabled or without capacity";
    else if (!Human(player) || player->GetTradeData())
        why = "an available online PvE player without group/trade is required";
    else if (!GoldRollRules::ParseTarget(request.target, low, stake) || stake > services.config.goldRollMaxCopper)
        why = "gold-roll target must be bot-counter:stake-copper within the configured maximum";
    else
    {
        ActivityCandidateQuery query;
        query.team = uint8(player->GetTeamId());
        query.minLevel = 1;
        query.maxLevel = 60;
        query.center = ActivityWorld::PointOf(player);
        query.sameMapOnly = true;
        query.maxDistance = NEAR_YARDS;
        query.rulesetSafe = true;
        query.limit = 32;
        for (auto const& candidate : services.candidates.Find(query))
        {
            if (candidate.guid.GetCounter() != low)
                continue;
            Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
            if (!Pair(bot, player, candidate.epoch) || bot->GetTradeData() || !Enough(bot, player, stake))
                continue;
            auto state = std::make_unique<RollState>();
            state->task.requester = player->GetGUID();
            state->task.stake = stake;
            state->bot = candidate.guid;
            state->map = player->GetMapId();
            plan.kind = Id();
            plan.gm = false;
            plan.requester = player->GetGUID();
            plan.team = uint8(player->GetTeamId());
            plan.minLevel = 1;
            plan.maxLevel = 60;
            plan.rally = query.center;
            plan.target = request.target;
            plan.name = "consented native gold roll";
            plan.needed = plan.wanted = 1;
            plan.leaderFixed = true;
            plan.candidates = {candidate};
            plan.decisions.clear();
            plan.state = std::move(state);
            return true;
        }
        why = "selected bot is unavailable, too far away, or either balance cannot cover the stake";
    }
    return false;
}

bool GoldRollKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    RollState* const state = dynamic_cast<RollState*>(plan.state.get());
    Player* const player = ObjectAccessor::FindConnectedPlayer(plan.requester);
    Player* const bot = state ? ObjectAccessor::FindConnectedPlayer(state->bot) : nullptr;
    if (!services.config.goldRollEnable || !state || state->task.stake > services.config.goldRollMaxCopper ||
        plan.candidates.size() != 1 || plan.decisions.size() != 1 || plan.decisions[0] != 1 ||
        !Pair(bot, player, plan.candidates.front().epoch) || !ActivityCandidates::IsFree(bot) ||
        bot->GetTradeData() || player->GetTradeData() || !Enough(bot, player, state->task.stake))
        return false;
    state->task.activity = activity.id;
    state->task.epoch = plan.candidates.front().epoch;
    state->task.untilMs = services.nowMs + CONSENT_MS;
    if (!Install(state->bot.GetCounter(), state->task, services.nowMs))
        return false;
    activity.requester = plan.requester;
    activity.leader = state->bot;
    activity.name = plan.name;
    activity.target = plan.target;
    activity.Enroll(state->bot, state->task.epoch, true, bot->GetLevel(), services.nowUnix);
    activity.hardDeadlineMs = state->task.untilMs;
    activity.SetCenter(plan.rally, NEAR_YARDS);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    activity.SetPhase(ActivityPhase::Recruiting, services.nowMs);
    state->line = ActivityWorld::Line("coa_act_gold_terms",
        "Bet #%id: %stake copper, dice 1-100, tie pays nobody. Loser pays through a money-only trade you accept. "
        "Agree within 60s: .playerbots activity gamble accept %id",
        {{"%id", std::to_string(activity.id)}, {"%stake", std::to_string(state->task.stake)}});
    state->lineUntilMs = services.nowMs + 15000;
    activity.state = std::move(plan.state);
    return true;
}

bool GoldRollKind::Accept(ActivityServices& services, Activity& activity, Player* player, std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    RollState* const state = dynamic_cast<RollState*>(activity.state.get());
    Player* const bot = state ? ObjectAccessor::FindConnectedPlayer(state->bot) : nullptr;
    if (!services.config.goldRollEnable || !state || activity.finishing || !player ||
        player->GetGUID() != activity.requester || !state->termsDelivered || state->task.accepted ||
        activity.phase != ActivityPhase::Recruiting || services.nowMs >= state->task.untilMs ||
        !Pair(bot, player, state->task.epoch) || !Enough(bot, player, state->task.stake) ||
        bot->GetTradeData() || player->GetTradeData())
    {
        why = "gold-roll proposal expired, already accepted, not yours, or participants are unavailable";
        return false;
    }
    state->task.accepted = true;
    state->task.botRoll = urand(1, 100);
    state->task.playerRoll = urand(1, 100);
    state->task.untilMs = services.nowMs + SETTLE_MS;
    Write(state->bot.GetCounter(), state->task);
    activity.hardDeadlineMs = state->task.untilMs;
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
    std::string const result = ActivityWorld::Line(
        state->task.botRoll == state->task.playerRoll ? "coa_act_gold_tie" :
        state->task.botRoll > state->task.playerRoll ? "coa_act_gold_player_pays" : "coa_act_gold_bot_pays",
        state->task.botRoll == state->task.playerRoll ? "Tie: no payment." :
        state->task.botRoll > state->task.playerRoll ? "You pay the stake." : "I pay the stake.");
    state->line = ActivityWorld::Line("coa_act_gold_result",
        "Bet #%id: my roll %botroll, yours %playerroll, stake %stake copper. %result Confirm the exact money-only trade.",
        {{"%id", std::to_string(activity.id)}, {"%botroll", std::to_string(state->task.botRoll)},
         {"%playerroll", std::to_string(state->task.playerRoll)}, {"%stake", std::to_string(state->task.stake)},
         {"%result", result}});
    state->lineUntilMs = services.nowMs + 15000;
    return true;
}

void GoldRollKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    RollState* const state = dynamic_cast<RollState*>(activity.state.get());
    ActivityMember const* const member = state ? activity.FindMember(state->bot) : nullptr;
    Player* const bot = member && member->In() ? ActivityWorld::FindMember(*member) : nullptr;
    Player* const player = ObjectAccessor::FindConnectedPlayer(activity.requester);
    if (!services.config.goldRollEnable)
        activity.Finish(ActivityOutcome::Disabled, "gold rolls disabled");
    else if (!state || !member || !Pair(bot, player, state->task.epoch) || player->GetMapId() != state->map)
        activity.Finish(ActivityOutcome::Guarded, "gold-roll participants became unavailable");
    else if (services.nowMs >= state->task.untilMs)
        activity.Finish(ActivityOutcome::Timeout, state->task.accepted ? "native gold-roll trade not settled in time" :
                                                                      "gold-roll proposal not accepted");
    else
    {
        RollTask task;
        if (!Read(state->bot.GetCounter(), task) || task.activity != activity.id || task.epoch != member->epoch)
            activity.Finish(ActivityOutcome::Stopped, "gold-roll task lost");
        else if (task.outcome == Outcome::Settled)
            activity.Finish(ActivityOutcome::Success, task.botRoll == task.playerRoll ? "tie; no gold moved" :
                                             "exact consented native money-only trade observed complete");
        else if (task.outcome == Outcome::Cancelled)
            activity.Finish(ActivityOutcome::Stopped, "gold-roll trade cancelled or exact settlement refused");
        else
            Post(services, activity, *state);
    }
}

void GoldRollKind::OnMemberLeft(ActivityServices&, Activity& activity, ActivityMember const&)
{
    activity.Finish(ActivityOutcome::Stopped, "gold-roll bot left");
}

std::string GoldRollKind::Join(ActivityServices&, Activity&, Player*)
{
    return "This proposal belongs to its requester. Use the explicit gold-roll accept command; no group invite.";
}

void GoldRollKind::OnEnd(ActivityServices&, Activity& activity)
{
    RollState* const state = dynamic_cast<RollState*>(activity.state.get());
    if (!state)
        return;
    std::lock_guard<std::mutex> lock(rollMutex);
    auto const found = rolls.find(state->bot.GetCounter());
    if (found != rolls.end() && found->second.activity == activity.id)
        found->second.cancelled = true;
}

std::string GoldRollKind::Describe(Activity const& activity, uint64 nowMs) const
{
    RollState const* const state = dynamic_cast<RollState const*>(activity.state.get());
    if (!state)
        return "gold-roll state missing";
    return Acore::StringFormat("requester {}, bot {}, stake {} copper, {}, rolls {}:{}, {} s left; native trade only",
                               activity.requester.GetCounter(), state->bot.GetCounter(), state->task.stake,
                               state->task.accepted ? "consented" : "awaiting explicit consent",
                               state->task.botRoll, state->task.playerRoll,
                               state->task.untilMs > nowMs ? (state->task.untilMs - nowMs) / 1000 : 0);
}

std::unique_ptr<ActivityKind> CreateGoldRollKind() { return std::make_unique<GoldRollKind>(); }
