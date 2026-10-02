/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityDuelRules.h"
#include "ActivityBrain.h"
#include "ActivityConfig.h"
#include "ActivityDirector.h"
#include "ActivityDuelCooldowns.h"
#include "ActivityRegistry.h"
#include "AiObjectContext.h"
#include "BrainService.h"
#include "CoaActivityValues.h"
#include "CoaMemoryRules.h"
#include "CoaTime.h"
#include "Log.h"
#include "MemoryView.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace
{
constexpr uint64 BRAIN_ASK_ACCOUNT_MS = 60 * 1000;  // one brain question per challenger account this often
constexpr uint64 BRAIN_ASK_BOT_MS = 30 * 1000;      // and per bot
constexpr uint64 BRAIN_ASK_PRUNE_MS = 60 * 1000;

// Rate limit of the brain duel questions: a challenge-then-cancel loop would otherwise pay for one question per
// challenge. Map threads (any bot, any challenger), so its own mutex; every critical section is a few map operations.
class BrainAskLimiter
{
public:
    static BrainAskLimiter& Instance()
    {
        static BrainAskLimiter instance;
        return instance;
    }

    // Checks and stamps both windows in one critical section. False (nothing stamped): the account or the bot was
    // asked about too recently.
    bool TryStamp(uint32 account, uint32 botLow, uint64 nowMs)
    {
        std::lock_guard<std::mutex> guard(lock);
        if (nowMs >= nextPruneMs)
        {
            std::erase_if(accountUntilMs, [nowMs](auto const& entry) { return entry.second <= nowMs; });
            std::erase_if(botUntilMs, [nowMs](auto const& entry) { return entry.second <= nowMs; });
            nextPruneMs = nowMs + BRAIN_ASK_PRUNE_MS;
        }

        auto const byAccount = accountUntilMs.find(account);
        if (byAccount != accountUntilMs.end() && nowMs < byAccount->second)
            return false;
        auto const byBot = botUntilMs.find(botLow);
        if (byBot != botUntilMs.end() && nowMs < byBot->second)
            return false;

        accountUntilMs[account] = nowMs + BRAIN_ASK_ACCOUNT_MS;
        botUntilMs[botLow] = nowMs + BRAIN_ASK_BOT_MS;
        return true;
    }

private:
    std::mutex lock;
    std::unordered_map<uint32, uint64> accountUntilMs;
    std::unordered_map<uint32, uint64> botUntilMs;
    uint64 nextPruneMs = 0;
};

uint8 BotHealth(PlayerbotAI* botAI)
{
    return botAI->GetAiObjectContext()->GetValue<uint8>("health", "self target")->Get();
}

// The challenge the bot was asked about is still the one it holds: challenged, not its own, from `challenger`.
bool IsPendingChallenge(Player* bot, ObjectGuid challenger)
{
    return bot->duel && bot->duel->State == DUEL_STATE_CHALLENGED && bot != bot->duel->Initiator &&
           bot->duel->Opponent && bot->duel->Opponent->GetGUID() == challenger;
}

// Accepts or declines the challenge the bot holds, as AcceptDuelAction does.
void Answer(PlayerbotAI* botAI, Player* bot, ObjectGuid flagGuid, bool accept)
{
    if (!accept)
    {
        WorldPacket packet(CMSG_DUEL_CANCELLED, 8);
        packet << flagGuid;
        bot->GetSession()->HandleDuelCancelledOpcode(packet);
        return;
    }

    WorldPacket packet(CMSG_DUEL_ACCEPTED, 8);
    packet << flagGuid;
    bot->GetSession()->HandleDuelAcceptedOpcode(packet);
    botAI->ResetStrategies();
}

// Hero's Call Board farming cap: duels this account fought against bots today reached Duel.MaxAcceptsPerPlayerPerDay.
bool AtDailyCap(ActivityConfig const& config, Player* player)
{
    if (!config.duelMaxAcceptsPerPlayerPerDay || !CoaActivityBot::HasRealSession(player))
        return false;

    uint32 const account = player->GetSession()->GetAccountId();
    return ActivityDuelCooldowns::Instance().RealDuelsToday(account, BrainClock::UtcDayNow()) >=
           config.duelMaxAcceptsPerPlayerPerDay;
}

ActivityBrain::DuelFeatures DuelFeaturesOf(PlayerbotAI* botAI, Player* bot, Player* challenger, uint8 health,
                                           ActivityMemberView const* row)
{
    ActivityBrain::DuelFeatures features;
    features.botLevel = bot->GetLevel();
    features.challengerLevel = challenger->GetLevel();
    features.botHealthPct = health;
    features.botClass = bot->getClass();
    features.challengerClass = challenger->getClass();
    features.bravery = ActivityTraits::Bravery(bot->GetGUID().GetCounter());
    features.inRing = row && row->kind == ActivityKindId::Duel;
    features.duelsToday = ActivityDuelCooldowns::Instance().RealDuelsToday(challenger->GetSession()->GetAccountId(),
                                                                           BrainClock::UtcDayNow());

    // The bot's memory of this player (null with memory off, or RivalDuels off).
    if (std::shared_ptr<MemoryView const> const view = CoaMemoryRules::ViewFor(botAI, MemoryBehaviour::RivalDuels))
    {
        if (MemoryRelationView const* relation = view->Find(challenger->GetGUID().GetCounter()))
        {
            MemoryLabel const label = view->Label(*relation, uint32(time(nullptr)));
            features.rival = label == MemoryLabel::Rival || label == MemoryLabel::Grudge;
            features.friendly = label == MemoryLabel::Friend;
        }
    }

    return features;
}
}  // namespace

ActivityDuelDecision ActivityDuelRules::OnChallenge(PlayerbotAI* botAI, ObjectGuid challenger, ObjectGuid flagGuid,
                                                    uint8 minHealth)
{
    // With activities off (or Duel.Enable = 0) this is the only statement: today's rule decides.
    if (!ActivityDirector::DuelRulesActive())
        return ActivityDuelDecision::Legacy;

    Player* bot = botAI->GetBot();
    Player* opponent = bot->duel ? bot->duel->Opponent : nullptr;
    if (!opponent || opponent->GetGUID() != challenger)
        return ActivityDuelDecision::Legacy;

    ActivityDuelCooldowns& duels = ActivityDuelCooldowns::Instance();
    uint32 const botLow = bot->GetGUID().GetCounter();
    uint64 const now = CoaTime::NowMs();

    // A new challenge: a brain question still out for an older one (its challenger cancelled) is withdrawn, so the
    // brain does not keep working on it. A waiting entry stays until BeginDecision or its deadline replaces it; with
    // none left the flag goes, or Continue's "nothing waits" branch would accept a later challenge unchecked.
    if (botAI->HasCoaDuelPending())
    {
        std::optional<ActivityDuelPending> const old = duels.TakeDecision(botLow, botAI->GetCoaEpoch(), now);
        if (old && old->jobId)
            BrainService::Current()->Cancel(old->jobId);
        if (!old || old->verdict != ActivityDuelVerdict::Waiting)
            botAI->SetCoaDuelPending(false);
    }

    std::shared_ptr<ActivityConfig const> const config = ActivityConfig::Current();

    // The master skips the health rule, never the daily cap: any player can make a random bot its master.
    Player* master = botAI->GetMaster();
    if (master && master->GetGUID() == challenger)
    {
        if (!AtDailyCap(*config, opponent))
            return ActivityDuelDecision::Legacy;

        LOG_DEBUG("playerbots.activity", "{} declines the duel of its master {}: the account's daily duels are used up",
                  bot->GetName(), opponent->GetName());
        return ActivityDuelDecision::Refuse;
    }

    bool const real = CoaActivityBot::HasRealSession(opponent);

    uint32 threshold = minHealth;
    if (real && config->duelPreferCallBoardHolders &&
        opponent->GetQuestStatus(ActivityThreshold::CALL_BOARD_DUEL_QUEST) == QUEST_STATUS_INCOMPLETE)
        threshold = std::min(threshold, config->duelCallBoardMinHp);

    uint8 const health = BotHealth(botAI);
    if (health < threshold)
    {
        LOG_DEBUG("playerbots.activity", "{} declines the duel of {}: health {} < {}", bot->GetName(),
                  opponent->GetName(), health, threshold);
        return ActivityDuelDecision::Refuse;
    }

    ActivityBotState& state = CoaActivityBot::Refresh(botAI);

    if (real)
    {
        if (AtDailyCap(*config, opponent))
        {
            LOG_DEBUG("playerbots.activity", "{} declines the duel of {}: the account's daily duels are used up",
                      bot->GetName(), opponent->GetName());
            return ActivityDuelDecision::Refuse;
        }

        std::shared_ptr<IBrain> const brain = BrainService::Current();
        if (!config->useBrainDuel || !brain->Active(BrainUse::Duel))
            return ActivityDuelDecision::Accept;

        // Asked about this account or this bot too recently: the rule without the brain (a stamp is spent even when
        // the brain then refuses the job).
        if (!BrainAskLimiter::Instance().TryStamp(opponent->GetSession()->GetAccountId(), botLow, now))
        {
            LOG_DEBUG("playerbots.activity", "{} accepts the duel of {}: brain asked too recently", bot->GetName(),
                      opponent->GetName());
            return ActivityDuelDecision::Accept;
        }

        uint64 const tag = duels.NextTag();
        uint64 const deadlineMs = now + config->brainDuelDeadlineMs;
        BrainJob job = ActivityBrain::BuildDuelJob(DuelFeaturesOf(botAI, bot, opponent, health, state.row),
                                                   bot->GetGUID().GetRawValue(), tag, deadlineMs);
        BrainTicket const ticket = brain->Submit(std::move(job), duels.Sink(), nullptr);
        if (ticket.status != BrainSubmit::Queued)
        {
            LOG_DEBUG("playerbots.activity", "{} accepts the duel of {}: brain {}", bot->GetName(),
                      opponent->GetName(), BrainSubmitName(ticket.status));
            return ActivityDuelDecision::Accept;
        }

        ActivityDuelPending pending;
        pending.epoch = botAI->GetCoaEpoch();
        pending.challenger = challenger;
        pending.flagGuid = flagGuid;
        pending.jobId = ticket.jobId;
        pending.tag = tag;
        pending.deadlineMs = deadlineMs;
        pending.verdict = ActivityDuelVerdict::Waiting;
        duels.BeginDecision(botLow, pending);
        botAI->SetCoaDuelPending(true);
        LOG_DEBUG("playerbots.activity", "{} asks the brain about the duel of {} (job {})", bot->GetName(),
                  opponent->GetName(), ticket.jobId);
        return ActivityDuelDecision::Pending;
    }

    // A bot challenger: ring partners always fight; anyone else depends on the bot's mood of the moment.
    if (ActivityMemberView const* row = state.row; row && row->kind == ActivityKindId::Duel && state.snapshot)
    {
        ActivityMemberView const* other = state.snapshot->Find(opponent->GetGUID().GetCounter());
        if (other && other->kind == ActivityKindId::Duel && other->activityId == row->activityId)
            return ActivityDuelDecision::Accept;
    }

    uint32 const mood = ActivityTraits::DuelMood(botLow, uint64(time(nullptr)));
    LOG_DEBUG("playerbots.activity", "{} {} the duel of {}: mood {}", bot->GetName(),
              mood >= ActivityThreshold::DUEL_MOOD_ACCEPT ? "accepts" : "declines", opponent->GetName(), mood);
    return mood >= ActivityThreshold::DUEL_MOOD_ACCEPT ? ActivityDuelDecision::Accept : ActivityDuelDecision::Refuse;
}

void ActivityDuelRules::Continue(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    std::optional<ActivityDuelPending> const decision = ActivityDuelCooldowns::Instance().TakeDecision(
        bot->GetGUID().GetCounter(), botAI->GetCoaEpoch(), CoaTime::NowMs());

    if (!decision)
    {
        // Nothing waits any more (the director stopped and cleared the store): today's rule, whose health check
        // was passed when the challenge came.
        botAI->SetCoaDuelPending(false);
        if (bot->duel && bot->duel->State == DUEL_STATE_CHALLENGED && bot != bot->duel->Initiator &&
            bot->duel->Opponent)
            Answer(botAI, bot, bot->duel->Opponent->GetGuidValue(PLAYER_DUEL_ARBITER), true);
        return;
    }

    if (decision->verdict == ActivityDuelVerdict::Waiting)
        return;

    botAI->SetCoaDuelPending(false);
    if (decision->verdict == ActivityDuelVerdict::Fallback && decision->jobId)
        BrainService::Current()->Cancel(decision->jobId);

    // The challenger may have cancelled, or the bot got another challenge meanwhile.
    if (!IsPendingChallenge(bot, decision->challenger))
        return;

    bool accept = decision->verdict != ActivityDuelVerdict::No;
    if (accept && ActivityDirector::DuelRulesActive() && AtDailyCap(*ActivityConfig::Current(), bot->duel->Opponent))
        accept = false;

    LOG_DEBUG("playerbots.activity", "{} {} the duel of {} ({})", bot->GetName(), accept ? "accepts" : "declines",
              bot->duel->Opponent->GetName(),
              decision->verdict == ActivityDuelVerdict::Yes  ? "brain yes"
              : decision->verdict == ActivityDuelVerdict::No ? "brain no"
                                                             : "no brain answer");
    Answer(botAI, bot, decision->flagGuid, accept);
}
