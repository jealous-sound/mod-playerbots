/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAPARTYSTRATEGY_H
#define PLAYERBOTS_COAPARTYSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// "coa party" (non-combat, M5): the participants of inn parties, city crowds and the Gurubashi booty run. Never
// added at runtime: CoaActivityStrategies::AddNonCombat adds it from the registry row's kind, so every
// ResetStrategies keeps it, and no other bot ever carries it (emotes only for participants). It emotes, sits and
// mounts; it never says a line (the kinds' lines are the director's operations).
//   "coa party greet" -> "coa party greet" 7
//   "coa party mount" -> "coa party mount" 6
//   "coa party emote" -> "coa party emote" 5
// Below "coa activity" travel, return and celebrate (13-16), so a participant first reaches its spot; above roam (3)
// and follow (1).
class CoaPartyStrategy : public Strategy
{
public:
    CoaPartyStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "coa party"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
