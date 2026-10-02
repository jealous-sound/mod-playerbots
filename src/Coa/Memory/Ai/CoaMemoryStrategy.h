/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYSTRATEGY_H
#define PLAYERBOTS_COAMEMORYSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// "coa memory" (non-combat). Added by AiFactory::AddDefaultNonCombatStrategies to random bots, next to "duel",
// when CoaMemoryConfig::AnyBehaviour(). Its presence is also what AcceptDuelAction, EnemyPlayerValue and the voice
// director test (with each behaviour's own switch), so "nc -coa memory" turns every memory behaviour off for one
// bot. Triggers, always both: "coa known player near" -> "coa greet known" (11.0), "coa friend near" ->
// "coa offer group" (10.5). The triggers test their switch (GreetByMemory, FriendInvites) every time, so a
// .reload config takes effect at once for bots that have the strategy; the strategy itself arrives with the
// bot's next login or strategy reset.
class CoaMemoryStrategy : public Strategy
{
public:
    CoaMemoryStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "coa memory"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
