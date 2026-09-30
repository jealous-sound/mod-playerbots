/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYRULES_H
#define PLAYERBOTS_COAMEMORYRULES_H

#include "CoaMemoryConfig.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <memory>

class MemoryView;
class Player;
class PlayerbotAI;

// What AcceptDuelAction does with a challenge (CoaMemory.RivalDuels).
enum class CoaDuelVerdict : uint8
{
    Legacy = 0,  // today's rule: the master always, anyone else from 90 % health
    Eager,       // rival, grudge or friend: accepted from MemoryThreshold::DUEL_EAGER_HEALTH (70 %)
    Refuse       // disliked, or fled the last two duels within a day: refused whatever the health
};

// Memory rules for code that runs on the bot's map thread (AcceptDuelAction, EnemyPlayerValue, the "coa memory"
// triggers). They read the bot's published MemoryView and nothing else of the memory.
namespace CoaMemoryRules
{
// The view to use for `behaviour`, or null: the switch is off (CoaMemoryConfig::BehaviourActive), the bot lacks
// the "coa memory" strategy in its non-combat engine, or it has no view. The one gate every behaviour goes
// through: null means "do exactly what the code did before M3".
std::shared_ptr<MemoryView const> ViewFor(PlayerbotAI* botAI, MemoryBehaviour behaviour);

// RivalDuels. Legacy when there is no view (ViewFor null), no relation with `challenger`, or the challenger is the
// bot's master. Refuse: decayed affinity <= DUEL_REFUSE_AFFINITY, annoyance >= DUEL_REFUSE_ANNOYANCE, or
// fledStreak >= DUEL_REFUSE_FLED with the newest flee (lastFledAt) within DUEL_FLED_WINDOW. Eager: label Rival,
// Grudge or Friend. Else Legacy.
CoaDuelVerdict DuelVerdict(PlayerbotAI* botAI, ObjectGuid challenger);

// Revenge. How much the bot wants `target` (0: not at all): the decayed grudge, when the label is Grudge and
// either they killed the bot (killed_me > 0) or a battleground grudge runs; 0 when `target`'s account opted out
// of hostile bots (COA_OPTOUT_HOSTILE). `now` in unix time.
int32 RevengeScore(MemoryView const& view, Player* target, uint32 now);
}  // namespace CoaMemoryRules

#endif
