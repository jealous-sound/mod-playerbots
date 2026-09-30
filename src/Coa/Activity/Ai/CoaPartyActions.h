/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAPARTYACTIONS_H
#define PLAYERBOTS_COAPARTYACTIONS_H

#include "Action.h"

class PlayerbotAI;

// The "coa party" actions (M5), the bot's map thread: emotes, sitting and mounting only. Never a chat line, an invite
// or a group change. Behaviour in the contract (section 10.2).

// "coa party emote": one emote of the row's style list, facing a participant of the same activity or a real player
// within PARTY_FACE_YARDS (the "coa duel emote" way: SMSG_TEXT_EMOTE through HandleTextEmoteOpcode), sometimes sitting
// down (INN); nextPartyEmoteMs = now + 15..40 s (DANCE: 40..70 s, the dance lasts until the bot moves).
class CoaPartyEmoteAction : public Action
{
public:
    CoaPartyEmoteAction(PlayerbotAI* botAI) : Action(botAI, "coa party emote") {}
    bool Execute(Event event) override;
};

// "coa party greet": stands up, faces greetTarget, waves or says hello (TEXT_EMOTE_WAVE / TEXT_EMOTE_HELLO), records
// greetedMs; clears greetTarget.
class CoaPartyGreetAction : public Action
{
public:
    CoaPartyGreetAction(PlayerbotAI* botAI) : Action(botAI, "coa party greet") {}
    bool Execute(Event event) override;
    bool isUseful() override;  // greetTarget is set
};

// "coa party mount": CheckMountStateAction::Mount() of the bot's own "check mount state" action (its best mount);
// nextMountMs = now + 60 s whatever the result.
class CoaPartyMountAction : public Action
{
public:
    CoaPartyMountAction(PlayerbotAI* botAI) : Action(botAI, "coa party mount") {}
    bool Execute(Event event) override;
};

#endif
