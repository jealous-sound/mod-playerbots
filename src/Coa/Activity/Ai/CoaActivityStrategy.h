/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAACTIVITYSTRATEGY_H
#define PLAYERBOTS_COAACTIVITYSTRATEGY_H

#include "Strategy.h"

class Engine;
class Player;
class PlayerbotAI;

// The activity strategies. None is ever added at runtime only: AiFactory adds them from the registry snapshot
// (CoaActivityStrategies below), after ChangeStrategy(randomBotNonCombatStrategies), so every ResetStrategies
// (accepting a duel or an invite, a master change, the director's strategiesDirty) keeps them. They move, fight and
// emote; they never invite, post, mark targets or change groups.

// "coa activity" (non-combat), every participant. Triggers -> actions (relevance):
//   "coa activity celebrate" -> "coa activity celebrate" 16
//   "coa activity travel"    -> "coa activity travel"    15
//   "coa activity hold"      -> "coa activity hold"      14
//   "coa activity away"      -> "coa activity return"    13
//   "coa activity catch up"  -> "coa activity catch up"  12
//   "coa activity roam"      -> "coa activity roam"      3
//   M5: "coa activity engage" -> "coa activity attack objective" 20
//   M5: "coa activity retreat" -> "coa activity fall back"      17 (RETREAT, out of combat too)
// Above NewRpg's status update (11, removed for participants anyway), grind's "attack anything" (4) and follow (1);
// below the pvp strategy's "attack enemy player" (55). M5: with ACTIVITY_VIEW_QUIET the movement triggers (travel,
// hold, away, catch up, roam, engage) stay quiet; celebrate still fires.
class CoaActivityStrategy : public Strategy
{
public:
    CoaActivityStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "coa activity"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// "coa activity combat" (combat), every participant: "coa activity objective" -> "coa activity attack objective" 45.
// Holds an objective target while engaged (M5 kinds); M4 kinds set none, so it never fires in M4.
// M5: "coa activity retreat" -> "coa activity fall back" 60 (ACTIVITY_VIEW_RETREAT: back to the rally, fighting or
// not; M4 kinds never set the flag).
class CoaActivityCombatStrategy : public Strategy
{
public:
    CoaActivityCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "coa activity combat"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// "coa duel" (non-combat): duel-ring participants, and every random bot when Duel.Ambient = 1.
//   "coa duel ended"      -> "coa duel emote"     16
//   "coa duel unanswered" -> "coa duel cancel"    9
//   "coa duel partner"    -> "coa challenge duel" 8
//   "coa ring idle"       -> "coa ring idle"      2
// Not "start duel", which a quarter of the random bots carry (AiFactory's 25 % draw) and stays empty.
class CoaDuelStrategy : public Strategy
{
public:
    CoaDuelStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "coa duel"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// AiFactory's calls, on whatever thread runs ResetStrategies or creates the engines (the bot's map thread, or the
// world thread at login). They read the registry snapshot (ActivityRegistry::Find) and the director's gates.
namespace CoaActivityStrategies
{
// AiFactory::AddDefaultNonCombatStrategies, at the end of the random-bot block (after both ChangeStrategy calls),
// only when ActivityDirector::Running(). A participant (row of this epoch): +"coa activity", -"lfg", -"bg"; one
// without a group or leading it (AiFactory's own test) also -"new rpg", -"rpg", -"move random", -"travel"; a
// duel-ring participant also -"grind", +"coa duel". Not a participant, no group, and ActivityDirector::DuelAmbient():
// +"coa duel". Strategies added and removed with init = false.
// M5: a party, loiter or gurubashi participant also -"grind", +"coa party"; a gurubashi participant with
// Gurubashi.BotsMayWin = 0 (ActivityConfig::Current()) also -"loot", -"gather" (the chest is the players'), and so
// does a former one (the last row its AI saw) within 40 yards of the chest while it is up (Gurubashi.Enable = 1).
// Every call first drops a stale activity objective from "prioritized targets"
// (CoaActivityAttackObjectiveAction::DropStale).
void AddNonCombat(Player* player, PlayerbotAI* facade, Engine* engine);

// AiFactory::AddDefaultCombatStrategies, right after the random bot's ChangeStrategy(randomBotCombatStrategies),
// only when ActivityDirector::Running(): a participant gets +"coa activity combat".
void AddCombat(Player* player, PlayerbotAI* facade, Engine* engine);
}  // namespace CoaActivityStrategies

#endif
