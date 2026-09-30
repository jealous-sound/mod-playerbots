/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAPARTYTRIGGERS_H
#define PLAYERBOTS_COAPARTYTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// The "coa party" triggers (M5). The bot's map thread: they read the bot's own Player, players on its map and its
// registry row (CoaActivityBot::Refresh), nothing else. Each is false unless the row's kind is Party, Loiter or
// Gurubashi, its phase is Active and the bot is alive and out of combat. Conditions in the contract (section 10.2).

// "coa party emote", every 3 s: not moving, nextPartyEmoteMs passed.
class CoaPartyEmoteTrigger : public Trigger
{
public:
    CoaPartyEmoteTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa party emote", 3 * 1000) {}
    bool IsActive() override;
};

// "coa party greet", every 2 s, style INN or DANCE only, the bot's sociability >= PARTY_GREET_SOCIABILITY: scans every
// 5 s (nextGreetScanMs) for a real player within PARTY_GREET_YARDS, in sight, this bot has not greeted for
// PARTY_GREET_REPEAT_MS; stores it in greetTarget.
class CoaPartyGreetTrigger : public Trigger
{
public:
    CoaPartyGreetTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa party greet", 2 * 1000) {}
    bool IsActive() override;
};

// "coa party mount", every 5 s, style LOITER only: the bot's mount trait (ActivityTraits::Trait(low, 0x40017) < 50),
// not mounted, not moving, within the row's radius of the center, nextMountMs passed.
class CoaPartyMountTrigger : public Trigger
{
public:
    CoaPartyMountTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa party mount", 5 * 1000) {}
    bool IsActive() override;
};

#endif
