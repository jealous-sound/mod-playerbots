/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaActivityTriggers.h"
#include "ActivityConfig.h"
#include "ActivityDirector.h"
#include "ActivityDuelCooldowns.h"
#include "ActivityRegistry.h"
#include "BrainTypes.h"
#include "CoaActivityValues.h"
#include "CoaMemoryRules.h"
#include "CoaPlayerPrefs.h"
#include "CoaTime.h"
#include "DBCStores.h"
#include "MemoryView.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "SocialMgr.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <ctime>
#include <memory>
#include <optional>
#include <vector>

namespace
{
constexpr float DUEL_SCAN_YARDS = 30.0f;        // partners this close (the bot walks within the spell's 10 yards)
constexpr float DUEL_RING_REACH = 6.0f;         // a ring bot's partner stands within the leash + this of the spot:
                                                // the walk to it (stopping 6 yards short) stays inside the leash
constexpr uint64 DUEL_APPROACH_MS = 15 * 1000;  // the walk to a chosen partner lasts at most this
constexpr uint8 DUEL_MIN_HEALTH = 90;           // both sides from this health percent (RpgDuelTrigger's rule)
constexpr uint8 DUEL_MIN_LEVEL = 3;
constexpr uint64 APPLAUSE_WINDOW_MS = 30 * 1000;  // a spectator reacts to a ring duel that ended this recently
constexpr uint64 OWN_DUEL_MARGIN_MS = 1000;       // a duelist does not applaud its own duel's ring row

// The core's rule (Spell::EffectDuel): an area without an entry allows duels.
bool AreaAllowsDuels(uint32 areaId)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
    return !area || (area->flags & AREA_FLAG_ALLOW_DUELS);
}
}  // namespace

// --- "coa activity" ---

bool CoaActivityTravelTrigger::IsActive()
{
    ActivityMemberView const* r = CoaActivityBot::Refresh(botAI).row;
    if (!r || bot->IsInCombat() || bot->IsInFlight())
        return false;

    // Another map (FLT_MAX): the director teleports.
    float const dist = CoaActivityBot::DistanceToCenter(bot, *r);
    if (dist == FLT_MAX || dist <= r->radius)
        return false;

    if (r->phase == ActivityPhase::Gathering)
        return !(r->role == ActivityRole::Leader && r->Has(ACTIVITY_VIEW_HOLD));

    if (r->phase == ActivityPhase::Marching)
        return r->role == ActivityRole::Leader && !r->Has(ACTIVITY_VIEW_HOLD);

    return false;
}

bool CoaActivityHoldTrigger::IsActive()
{
    ActivityMemberView const* r = CoaActivityBot::Refresh(botAI).row;
    return r && r->role == ActivityRole::Leader && r->Has(ACTIVITY_VIEW_HOLD) && !bot->IsInCombat();
}

bool CoaActivityAwayTrigger::IsActive()
{
    ActivityMemberView const* r = CoaActivityBot::Refresh(botAI).row;
    if (!r || r->phase != ActivityPhase::Active || CoaActivityBot::IsFollowing(*r) || bot->IsInCombat() || bot->duel)
        return false;

    float const dist = CoaActivityBot::DistanceToCenter(bot, *r);
    return dist != FLT_MAX && dist > r->radius;
}

bool CoaActivityCatchUpTrigger::IsActive()
{
    ActivityMemberView const* r = CoaActivityBot::Refresh(botAI).row;
    if (!r || !CoaActivityBot::IsFollowing(*r) || r->phase <= ActivityPhase::Gathering || bot->IsInCombat())
        return false;

    Player* leader = CoaActivityBot::LeaderOnMap(bot, *r);
    return leader && bot->GetExactDist2d(leader) > ActivityThreshold::CATCH_UP_YARDS;
}

bool CoaActivityCelebrateTrigger::IsActive()
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    ActivityMemberView const* r = s.row;
    return r && r->Has(ACTIVITY_VIEW_CELEBRATE) && s.celebratedActivity != r->activityId && bot->IsAlive() &&
           !bot->IsInCombat();
}

bool CoaActivityRoamTrigger::IsActive()
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    ActivityMemberView const* r = s.row;
    if (!r || !r->Has(ACTIVITY_VIEW_HUNT) || CoaActivityBot::IsFollowing(*r) || bot->IsInCombat() ||
        bot->isMoving() || CoaTime::NowMs() < s.nextRoamMs)
        return false;

    // Grind found something to hunt: it goes first.
    return !AI_VALUE(Unit*, "grind target");
}

// --- "coa duel" ---

bool CoaDuelPartnerTrigger::IsActive()
{
    if (!ActivityDirector::DuelRulesActive())
        return false;

    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    ActivityMemberView const* r = s.row;
    bool const ring = r && r->kind == ActivityKindId::Duel && r->Has(ACTIVITY_VIEW_MAY_CHALLENGE);
    bool const ambient = !r && ActivityDirector::DuelAmbient();
    if (!ring && !ambient)
    {
        s.duelTarget.Clear();
        return false;
    }

    uint64 const now = CoaTime::NowMs();

    // A partner was chosen: the action walks within the spell's reach, then challenges.
    if (!s.duelTarget.IsEmpty())
    {
        if (!bot->duel && now < s.duelApproachUntilMs)
            return true;
        s.duelTarget.Clear();
    }

    if (now < s.nextDuelScanMs)
        return false;
    s.nextDuelScanMs = now + urand(20000, 40000);

    // The challenger.
    if (!bot->IsAlive() || bot->IsInCombat() || bot->duel || bot->GetLevel() < DUEL_MIN_LEVEL ||
        bot->IsInFlight() || bot->GetGroup() || botAI->HasGameClientMaster() || !AreaAllowsDuels(bot->GetAreaId()))
        return false;
    if (AI_VALUE2(uint8, "health", "self target") < DUEL_MIN_HEALTH)
        return false;

    ActivityDuelCooldowns& duels = ActivityDuelCooldowns::Instance();
    uint32 const botLow = bot->GetGUID().GetCounter();
    uint64 const lastOwnEnd = duels.LastOwnDuelEndMs(botLow);
    if (lastOwnEnd && now < lastOwnEnd + ActivityThreshold::BOT_DUEL_REST_MS)
        return false;

    std::shared_ptr<ActivityConfig const> const config = ActivityConfig::Current();
    std::shared_ptr<MemoryView const> const view = CoaMemoryRules::ViewFor(botAI, MemoryBehaviour::RivalDuels);
    CoaPlayerPrefs const& prefs = CoaPlayerPrefs::Instance();
    uint32 const today = BrainClock::UtcDayNow();
    uint32 const nowUnix = uint32(time(nullptr));

    // Preference tiers: Hero's Call Board holders (when preferred), remembered rivals and friends, everyone else.
    std::array<std::vector<Player*>, 3> tiers;
    GuidVector const nearby = AI_VALUE(GuidVector, "nearest friendly players");
    for (ObjectGuid const& guid : nearby)
    {
        Player* p = ObjectAccessor::GetPlayer(*bot, guid);
        if (!p || p == bot || p->IsBeingTeleported() || !p->IsAlive() || p->IsInCombat() || p->duel ||
            p->IsInFlight() || p->IsGameMaster() || p->GetTeamId() != bot->GetTeamId())
            continue;
        if (p->GetLevel() > bot->GetLevel() + 3 || bot->GetLevel() > p->GetLevel() + 10)
            continue;
        if (bot->GetExactDist2d(p) > DUEL_SCAN_YARDS || !bot->IsWithinLOSInMap(p) || !AreaAllowsDuels(p->GetAreaId()))
            continue;
        if (!p->GetSocial() || p->GetSocial()->HasIgnore(bot->GetGUID()))
            continue;

        if (ring && CoaActivityBot::DistanceToCenter(p, *r) > r->radius + DUEL_RING_REACH)
            continue;

        uint32 const low = p->GetGUID().GetCounter();
        if (!duels.PairReady(botLow, low, now))
            continue;

        bool const real = CoaActivityBot::HasRealSession(p);
        if (!real)
        {
            if (p->GetHealthPct() < float(DUEL_MIN_HEALTH))
                continue;
            // A ring bot duels its own ring; an ambient bot leaves every participant alone.
            ActivityMemberView const* other = s.snapshot ? s.snapshot->Find(low) : nullptr;
            if (ring ? (!other || other->activityId != r->activityId) : other != nullptr)
                continue;
        }
        else
        {
            if (ring && !r->Has(ACTIVITY_VIEW_MAY_CHALLENGE_REAL))
                continue;
            if (p->isAFK() || p->isDND())
                continue;
            uint32 const account = p->GetSession()->GetAccountId();
            if (!prefs.IsLoaded() || (prefs.Get(account).activityMask & COA_OPTOUT_DUELS))
                continue;
            if (!duels.PlayerReady(account, now))
                continue;
            if (config->duelMaxAcceptsPerPlayerPerDay &&
                duels.RealDuelsToday(account, today) >= config->duelMaxAcceptsPerPlayerPerDay)
                continue;
        }

        std::size_t tier = 2;
        if (real && config->duelPreferCallBoardHolders &&
            p->GetQuestStatus(ActivityThreshold::CALL_BOARD_DUEL_QUEST) == QUEST_STATUS_INCOMPLETE)
            tier = 0;
        else if (view)
        {
            if (MemoryRelationView const* relation = view->Find(low))
            {
                MemoryLabel const label = view->Label(*relation, nowUnix);
                if (label == MemoryLabel::Rival || label == MemoryLabel::Friend)
                    tier = 1;
            }
        }
        tiers[tier].push_back(p);
    }

    for (std::vector<Player*> const& tier : tiers)
    {
        if (tier.empty())
            continue;

        Player* partner = tier[urand(0, uint32(tier.size()) - 1)];
        s.duelTarget = partner->GetGUID();
        s.duelApproachUntilMs = now + DUEL_APPROACH_MS;
        LOG_DEBUG("playerbots.activity", "{} picks {} for a duel ({})", bot->GetName(), partner->GetName(),
                  ring ? "ring" : "ambient");
        return true;
    }

    return false;
}

bool CoaDuelEndedTrigger::IsActive()
{
    if (!ActivityDirector::DuelRulesActive())
        return false;

    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    if (s.pendingEmote)
        return true;

    if (std::optional<ActivityDuelOutcome> const outcome =
            ActivityDuelCooldowns::Instance().TakeOutcome(bot->GetGUID().GetCounter()))
    {
        // Its own duel is also the ring's latest: no applause for it.
        s.seenRingDuelEndMs = std::max(s.seenRingDuelEndMs, outcome->endMs + OWN_DUEL_MARGIN_MS);
        switch (outcome->kind)
        {
            case ActivityDuelOutcomeKind::Won:
                s.pendingEmote = TEXT_EMOTE_CHEER;
                s.emoteFace = outcome->opponent;
                break;
            case ActivityDuelOutcomeKind::Lost:
                s.pendingEmote = urand(0, 2) ? TEXT_EMOTE_BOW : TEXT_EMOTE_CRY;
                s.emoteFace = outcome->opponent;
                break;
            case ActivityDuelOutcomeKind::Fled:
                break;
        }
        return s.pendingEmote != 0;
    }

    // A ring spectator: half the time it applauds a duel between ring members that just ended.
    ActivityMemberView const* r = s.row;
    if (r && r->kind == ActivityKindId::Duel && r->ringDuelEndMs > s.seenRingDuelEndMs)
    {
        s.seenRingDuelEndMs = r->ringDuelEndMs;
        uint64 const now = CoaTime::NowMs();
        if (now < r->ringDuelEndMs + APPLAUSE_WINDOW_MS && !bot->duel && !bot->IsInCombat() && urand(0, 1))
        {
            s.pendingEmote = TEXT_EMOTE_APPLAUD;
            s.emoteFace.Clear();
        }
    }

    return s.pendingEmote != 0;
}

bool CoaDuelUnansweredTrigger::IsActive()
{
    if (!ActivityDirector::DuelRulesActive())
        return false;

    ActivityBotState& s = AI_VALUE(ActivityBotState&, "coa activity state");
    if (!s.challengeSentMs)
        return false;

    // Still its own challenge, unanswered: withdrawn after CHALLENGE_TIMEOUT_MS. Anything else: forgotten.
    if (bot->duel && bot->duel->Initiator == bot && bot->duel->State == DUEL_STATE_CHALLENGED)
        return CoaTime::NowMs() >= s.challengeSentMs + ActivityThreshold::CHALLENGE_TIMEOUT_MS;

    s.challengeSentMs = 0;
    return false;
}

bool CoaRingIdleTrigger::IsActive()
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    ActivityMemberView const* r = s.row;
    if (!r || r->kind != ActivityKindId::Duel || r->phase != ActivityPhase::Active || bot->IsInCombat() ||
        bot->duel || CoaTime::NowMs() < s.nextIdleMs)
        return false;

    return CoaActivityBot::DistanceToCenter(bot, *r) <= r->radius;
}

// --- "coa activity combat" ---

bool CoaActivityObjectiveTrigger::IsActive()
{
    ActivityMemberView const* r = CoaActivityBot::Refresh(botAI).row;
    if (!r || r->objective.IsEmpty())
        return false;

    Unit* objective = CoaActivityBot::ObjectiveInReach(botAI, *r);
    return objective && AI_VALUE(Unit*, "current target") != objective;
}
