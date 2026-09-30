/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAACTIVITYAICONTEXT_H
#define PLAYERBOTS_COAACTIVITYAICONTEXT_H

#include "Action.h"
#include "CoaActivityActions.h"
#include "CoaActivityStrategy.h"
#include "CoaActivityTriggers.h"
#include "CoaActivityValues.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"

class PlayerbotAI;

// The activity objects, registered for every class by AiObjectContext::BuildSharedStrategyContexts,
// BuildSharedTriggerContexts, BuildSharedActionContexts and BuildSharedValueContexts (src/Bot/Engine/BuildShared*.cpp)
// after the CoA memory contexts. No siblings: adding one "coa *" strategy never takes another strategy off.

class CoaActivityStrategyContext : public NamedObjectContext<Strategy>
{
public:
    CoaActivityStrategyContext() : NamedObjectContext<Strategy>(false, false)
    {
        creators["coa activity"] = &CoaActivityStrategyContext::coa_activity;
        creators["coa activity combat"] = &CoaActivityStrategyContext::coa_activity_combat;
        creators["coa duel"] = &CoaActivityStrategyContext::coa_duel;
    }

private:
    static Strategy* coa_activity(PlayerbotAI* botAI) { return new CoaActivityStrategy(botAI); }
    static Strategy* coa_activity_combat(PlayerbotAI* botAI) { return new CoaActivityCombatStrategy(botAI); }
    static Strategy* coa_duel(PlayerbotAI* botAI) { return new CoaDuelStrategy(botAI); }
};

class CoaActivityTriggerContext : public NamedObjectContext<Trigger>
{
public:
    CoaActivityTriggerContext()
    {
        creators["coa activity travel"] = &CoaActivityTriggerContext::travel;
        creators["coa activity hold"] = &CoaActivityTriggerContext::hold;
        creators["coa activity away"] = &CoaActivityTriggerContext::away;
        creators["coa activity catch up"] = &CoaActivityTriggerContext::catch_up;
        creators["coa activity celebrate"] = &CoaActivityTriggerContext::celebrate;
        creators["coa activity roam"] = &CoaActivityTriggerContext::roam;
        creators["coa duel partner"] = &CoaActivityTriggerContext::duel_partner;
        creators["coa duel ended"] = &CoaActivityTriggerContext::duel_ended;
        creators["coa duel unanswered"] = &CoaActivityTriggerContext::duel_unanswered;
        creators["coa ring idle"] = &CoaActivityTriggerContext::ring_idle;
        creators["coa activity objective"] = &CoaActivityTriggerContext::objective;
        creators["coa activity engage"] = &CoaActivityTriggerContext::engage;
        creators["coa activity retreat"] = &CoaActivityTriggerContext::retreat;
    }

private:
    static Trigger* travel(PlayerbotAI* botAI) { return new CoaActivityTravelTrigger(botAI); }
    static Trigger* hold(PlayerbotAI* botAI) { return new CoaActivityHoldTrigger(botAI); }
    static Trigger* away(PlayerbotAI* botAI) { return new CoaActivityAwayTrigger(botAI); }
    static Trigger* catch_up(PlayerbotAI* botAI) { return new CoaActivityCatchUpTrigger(botAI); }
    static Trigger* celebrate(PlayerbotAI* botAI) { return new CoaActivityCelebrateTrigger(botAI); }
    static Trigger* roam(PlayerbotAI* botAI) { return new CoaActivityRoamTrigger(botAI); }
    static Trigger* duel_partner(PlayerbotAI* botAI) { return new CoaDuelPartnerTrigger(botAI); }
    static Trigger* duel_ended(PlayerbotAI* botAI) { return new CoaDuelEndedTrigger(botAI); }
    static Trigger* duel_unanswered(PlayerbotAI* botAI) { return new CoaDuelUnansweredTrigger(botAI); }
    static Trigger* ring_idle(PlayerbotAI* botAI) { return new CoaRingIdleTrigger(botAI); }
    static Trigger* objective(PlayerbotAI* botAI) { return new CoaActivityObjectiveTrigger(botAI); }
    static Trigger* engage(PlayerbotAI* botAI) { return new CoaActivityEngageTrigger(botAI); }
    static Trigger* retreat(PlayerbotAI* botAI) { return new CoaActivityRetreatTrigger(botAI); }
};

class CoaActivityActionContext : public NamedObjectContext<Action>
{
public:
    CoaActivityActionContext()
    {
        creators["coa activity travel"] = &CoaActivityActionContext::travel;
        creators["coa activity hold"] = &CoaActivityActionContext::hold;
        creators["coa activity return"] = &CoaActivityActionContext::ret;
        creators["coa activity catch up"] = &CoaActivityActionContext::catch_up;
        creators["coa activity celebrate"] = &CoaActivityActionContext::celebrate;
        creators["coa activity roam"] = &CoaActivityActionContext::roam;
        creators["coa challenge duel"] = &CoaActivityActionContext::challenge_duel;
        creators["coa duel emote"] = &CoaActivityActionContext::duel_emote;
        creators["coa duel cancel"] = &CoaActivityActionContext::duel_cancel;
        creators["coa ring idle"] = &CoaActivityActionContext::ring_idle;
        creators["coa activity attack objective"] = &CoaActivityActionContext::attack_objective;
        creators["coa activity fall back"] = &CoaActivityActionContext::fall_back;
    }

private:
    static Action* travel(PlayerbotAI* botAI) { return new CoaActivityTravelAction(botAI); }
    static Action* hold(PlayerbotAI* botAI) { return new CoaActivityHoldAction(botAI); }
    static Action* ret(PlayerbotAI* botAI) { return new CoaActivityReturnAction(botAI); }
    static Action* catch_up(PlayerbotAI* botAI) { return new CoaActivityCatchUpAction(botAI); }
    static Action* celebrate(PlayerbotAI* botAI) { return new CoaActivityCelebrateAction(botAI); }
    static Action* roam(PlayerbotAI* botAI) { return new CoaActivityRoamAction(botAI); }
    static Action* challenge_duel(PlayerbotAI* botAI) { return new CoaChallengeDuelAction(botAI); }
    static Action* duel_emote(PlayerbotAI* botAI) { return new CoaDuelEmoteAction(botAI); }
    static Action* duel_cancel(PlayerbotAI* botAI) { return new CoaDuelCancelAction(botAI); }
    static Action* ring_idle(PlayerbotAI* botAI) { return new CoaRingIdleAction(botAI); }
    static Action* attack_objective(PlayerbotAI* botAI) { return new CoaActivityAttackObjectiveAction(botAI); }
    static Action* fall_back(PlayerbotAI* botAI) { return new CoaActivityFallBackAction(botAI); }
};

class CoaActivityValueContext : public NamedObjectContext<UntypedValue>
{
public:
    CoaActivityValueContext() { creators["coa activity state"] = &CoaActivityValueContext::activity_state; }

private:
    static UntypedValue* activity_state(PlayerbotAI* botAI) { return new CoaActivityStateValue(botAI); }
};

#endif
