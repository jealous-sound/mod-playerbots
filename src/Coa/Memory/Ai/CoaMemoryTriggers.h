/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYTRIGGERS_H
#define PLAYERBOTS_COAMEMORYTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// Both run on the bot's map thread and read only the bot's own Player, players on its map, its published
// MemoryView and CoaPlayerPrefs (shared lock). They return false at once when their switch is off
// (CoaMemoryConfig::BehaviourActive) or the bot has no view.

// "coa known player near", every 5 s (CoaMemory.GreetByMemory). A real player the bot knows (label Friend, Rival,
// Grudge, or Acquaintance) is close; picks one and fills CoaMemoryBotState::greetTarget and greetLabel. Rules in
// the contract (distance, cooldowns, opt-outs).
class CoaKnownPlayerNearTrigger : public Trigger
{
public:
    CoaKnownPlayerNearTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa known player near", 5 * 1000) {}

    bool IsActive() override;
};

// "coa friend near", every 10 s (CoaMemory.FriendInvites). A real player of label Friend, ungrouped like the bot,
// close and of a near level; fills CoaMemoryBotState::offerTarget. Rules in the contract.
class CoaFriendNearTrigger : public Trigger
{
public:
    CoaFriendNearTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa friend near", 10 * 1000) {}

    bool IsActive() override;
};

#endif
