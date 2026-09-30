/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYCONTEXT_H
#define PLAYERBOTS_COAMEMORYCONTEXT_H

#include "Action.h"
#include "CoaMemoryActions.h"
#include "CoaMemoryStrategy.h"
#include "CoaMemoryTriggers.h"
#include "CoaMemoryValues.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"

class PlayerbotAI;

// The "coa memory" objects, registered for every class by AiObjectContext::BuildSharedStrategyContexts,
// BuildSharedTriggerContexts, BuildSharedActionContexts and BuildSharedValueContexts (src/Bot/Engine/BuildShared*.cpp)
// after the CoA generic contexts. No siblings: adding "coa memory" never takes another strategy off.

class CoaMemoryStrategyContext : public NamedObjectContext<Strategy>
{
public:
    CoaMemoryStrategyContext() : NamedObjectContext<Strategy>(false, false)
    {
        creators["coa memory"] = &CoaMemoryStrategyContext::coa_memory;
    }

private:
    static Strategy* coa_memory(PlayerbotAI* botAI) { return new CoaMemoryStrategy(botAI); }
};

class CoaMemoryTriggerContext : public NamedObjectContext<Trigger>
{
public:
    CoaMemoryTriggerContext()
    {
        creators["coa known player near"] = &CoaMemoryTriggerContext::known_player_near;
        creators["coa friend near"] = &CoaMemoryTriggerContext::friend_near;
    }

private:
    static Trigger* known_player_near(PlayerbotAI* botAI) { return new CoaKnownPlayerNearTrigger(botAI); }
    static Trigger* friend_near(PlayerbotAI* botAI) { return new CoaFriendNearTrigger(botAI); }
};

class CoaMemoryActionContext : public NamedObjectContext<Action>
{
public:
    CoaMemoryActionContext()
    {
        creators["coa greet known"] = &CoaMemoryActionContext::greet_known;
        creators["coa offer group"] = &CoaMemoryActionContext::offer_group;
    }

private:
    static Action* greet_known(PlayerbotAI* botAI) { return new CoaGreetKnownAction(botAI); }
    static Action* offer_group(PlayerbotAI* botAI) { return new CoaOfferGroupAction(botAI); }
};

class CoaMemoryValueContext : public NamedObjectContext<UntypedValue>
{
public:
    CoaMemoryValueContext() { creators["coa memory state"] = &CoaMemoryValueContext::memory_state; }

private:
    static UntypedValue* memory_state(PlayerbotAI* botAI) { return new CoaMemoryStateValue(botAI); }
};

#endif
