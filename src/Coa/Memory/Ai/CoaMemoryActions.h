/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYACTIONS_H
#define PLAYERBOTS_COAMEMORYACTIONS_H

#include "Action.h"

class PlayerbotAI;

// The bot's map thread. The bot's own speech and emotes only (Say, PlayEmote, Whisper to one player on its map):
// never an invite, a group change or a channel post.

// "coa greet known": faces CoaMemoryBotState::greetTarget, plays the label's emote and says a coa_mem_greet_* line
// (see the contract), stamps greetedAt and lastGreetAt, and adds the player to "already seen players" so the stock
// greeting stays quiet. Clears greetTarget.
class CoaGreetKnownAction : public Action
{
public:
    CoaGreetKnownAction(PlayerbotAI* botAI) : Action(botAI, "coa greet known") {}

    bool Execute(Event event) override;
    bool isUseful() override;  // a greetTarget is set
};

// "coa offer group": whispers a coa_mem_invite_offer line to CoaMemoryBotState::offerTarget (the player then
// invites; AcceptInvitationAction accepts as usual), stamps offeredAt and lastOfferAt. Clears offerTarget.
class CoaOfferGroupAction : public Action
{
public:
    CoaOfferGroupAction(PlayerbotAI* botAI) : Action(botAI, "coa offer group") {}

    bool Execute(Event event) override;
    bool isUseful() override;  // an offerTarget is set
};

#endif
