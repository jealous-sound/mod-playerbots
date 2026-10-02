/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAPARTYAICONTEXT_H
#define PLAYERBOTS_COAPARTYAICONTEXT_H

#include "Action.h"
#include "CoaPartyActions.h"
#include "CoaPartyStrategy.h"
#include "CoaPartyTriggers.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"

class PlayerbotAI;

// The "coa party" objects (M5), registered for every class by AiObjectContext::BuildSharedStrategyContexts,
// BuildSharedTriggerContexts and BuildSharedActionContexts (src/Bot/Engine/BuildShared*.cpp) right after the CoA
// activity contexts. No value of their own: they use "coa activity state". No siblings.

class CoaPartyStrategyContext : public NamedObjectContext<Strategy>
{
public:
    CoaPartyStrategyContext() : NamedObjectContext<Strategy>(false, false)
    {
        creators["coa party"] = &CoaPartyStrategyContext::coa_party;
    }

private:
    static Strategy* coa_party(PlayerbotAI* botAI) { return new CoaPartyStrategy(botAI); }
};

class CoaPartyTriggerContext : public NamedObjectContext<Trigger>
{
public:
    CoaPartyTriggerContext()
    {
        creators["coa party emote"] = &CoaPartyTriggerContext::emote;
        creators["coa party greet"] = &CoaPartyTriggerContext::greet;
        creators["coa party mount"] = &CoaPartyTriggerContext::mount;
    }

private:
    static Trigger* emote(PlayerbotAI* botAI) { return new CoaPartyEmoteTrigger(botAI); }
    static Trigger* greet(PlayerbotAI* botAI) { return new CoaPartyGreetTrigger(botAI); }
    static Trigger* mount(PlayerbotAI* botAI) { return new CoaPartyMountTrigger(botAI); }
};

class CoaPartyActionContext : public NamedObjectContext<Action>
{
public:
    CoaPartyActionContext()
    {
        creators["coa party emote"] = &CoaPartyActionContext::emote;
        creators["coa party greet"] = &CoaPartyActionContext::greet;
        creators["coa party mount"] = &CoaPartyActionContext::mount;
    }

private:
    static Action* emote(PlayerbotAI* botAI) { return new CoaPartyEmoteAction(botAI); }
    static Action* greet(PlayerbotAI* botAI) { return new CoaPartyGreetAction(botAI); }
    static Action* mount(PlayerbotAI* botAI) { return new CoaPartyMountAction(botAI); }
};

#endif
