/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYDUELRULES_H
#define PLAYERBOTS_ACTIVITYDUELRULES_H

#include "Define.h"
#include "ObjectGuid.h"

class PlayerbotAI;

// What AcceptDuelAction does after the memory rule (CoaMemoryRules::DuelVerdict).
enum class ActivityDuelDecision : uint8
{
    Legacy = 0,  // the activity rules are off, or the challenger is the bot's master below the cap: today's rule
    Accept,      // accept now
    Refuse,      // cancel now
    Pending,     // a brain question is out: ActivityDuelRules::Continue answers later (the flag is set)
};

// The layered duel accept (contract section 12.4). The bot's map thread only.
namespace ActivityDuelRules
{
// AcceptDuelAction, after the memory verdict (never called for Refuse). `minHealth`: 90, or the memory's Eager
// threshold. First statement: `if (!ActivityDirector::DuelRulesActive()) return Legacy;` (one relaxed load).
// Then: a brain question still out for an older challenge -> its job is cancelled. The master: its account at
// Duel.MaxAcceptsPerPlayerPerDay duels today -> Refuse, else Legacy (the master skips the health rule, never the
// cap). Health below minHealth (below min(minHealth, Duel.CallBoardMinHp) for a real challenger holding quest 80651
// incomplete when PreferCallBoardHolders = 1) -> Refuse. A real challenger: its account at the cap -> Refuse;
// CoaBrain.Use.Duel = 1 and the brain active for BrainUse::Duel -> asked at most once a minute per account and every
// 30 s per bot (else Accept, the rule without the brain), then the job is submitted: Queued -> BeginDecision,
// SetCoaDuelPending(true), Pending; anything else -> Accept. A bot challenger: a member of the same duel ring ->
// Accept; else DuelMood >= DUEL_MOOD_ACCEPT -> Accept, else Refuse.
ActivityDuelDecision OnChallenge(PlayerbotAI* botAI, ObjectGuid challenger, ObjectGuid flagGuid, uint8 minHealth);

// PlayerbotAI::UpdateAIInternal, before HandleCommands, when HasCoaDuelPending(). Takes the decision
// (ActivityDuelCooldowns::TakeDecision): still Waiting -> nothing. Otherwise clears the flag; cancels the brain job of
// a Fallback; checks the duel is still this challenge (bot->duel, DUEL_STATE_CHALLENGED, the bot is not the initiator,
// the opponent is the challenger): Yes or Fallback -> accept (cap re-checked: at the cap -> cancel) with
// HandleDuelAcceptedOpcode and ResetStrategies, as AcceptDuelAction does; No -> cancel (CMSG_DUEL_CANCELLED). No
// decision at all (the director stopped meanwhile) -> the flag is cleared and a pending challenge is accepted
// (today's rule: the health was checked when it came).
void Continue(PlayerbotAI* botAI);
}  // namespace ActivityDuelRules

#endif
