/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYVALUES_H
#define PLAYERBOTS_COAMEMORYVALUES_H

#include "MemoryTypes.h"
#include "ObjectGuid.h"
#include "Value.h"
#include <unordered_map>

class PlayerbotAI;

// Per-bot state of the "coa memory" strategy: its triggers choose, its actions act and stamp. Touched only by the
// bot's own AI objects on its map thread; never saved (Save() stays "?"), so it restarts empty at each login.
struct CoaMemoryBotState
{
    ObjectGuid greetTarget;                          // set by "coa known player near" for "coa greet known"
    MemoryLabel greetLabel = MemoryLabel::Stranger;  // how that player is known
    std::unordered_map<uint32, uint32> greetedAt;    // other low guid -> unix time of the last memory greeting
    uint32 lastGreetAt = 0;                          // this bot's last memory greeting, anyone
    ObjectGuid offerTarget;                          // set by "coa friend near" for "coa offer group"
    std::unordered_map<uint32, uint32> offeredAt;    // other low guid -> unix time of the last group offer
    uint32 lastOfferAt = 0;                          // this bot's last offer, anyone
};

// "coa memory state".
class CoaMemoryStateValue : public ManualSetValue<CoaMemoryBotState&>
{
public:
    CoaMemoryStateValue(PlayerbotAI* botAI) : ManualSetValue<CoaMemoryBotState&>(botAI, data, "coa memory state") {}

    CoaMemoryBotState data;
};

#endif
