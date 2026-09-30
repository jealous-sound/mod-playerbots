/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAACTIVITYACTIONS_H
#define PLAYERBOTS_COAACTIVITYACTIONS_H

#include "Action.h"
#include "ActivityMoveFarTo.h"
#include "AttackAction.h"
#include "ObjectGuid.h"

class PlayerbotAI;
struct ActivityMemberView;

// The bot's map thread. They move the bot, attack, cast the duel spell and play emotes: never an invite, a group
// change, a raid icon, a chat line or a channel post (those are the director's operations). Behaviour in the
// contract (section 10.4).

// "coa activity travel": ActivityMoveFarTo to a point jittered 0.3..1 x RALLY_ARRIVE_RADIUS around the center
// (Gathering), or to the center (the leader, Marching).
class CoaActivityTravelAction : public ActivityBaseAction
{
public:
    CoaActivityTravelAction(PlayerbotAI* botAI) : ActivityBaseAction(botAI, "coa activity travel") {}
    bool Execute(Event event) override;
    bool isUseful() override;  // still a participant, not in combat, not in flight
};

// "coa activity hold": ForceToWait(3000) where the bot stands.
class CoaActivityHoldAction : public ActivityBaseAction
{
public:
    CoaActivityHoldAction(PlayerbotAI* botAI) : ActivityBaseAction(botAI, "coa activity hold") {}
    bool Execute(Event event) override;
};

// "coa activity return": ActivityMoveFarTo to a point within half the leash radius of the center.
class CoaActivityReturnAction : public ActivityBaseAction
{
public:
    CoaActivityReturnAction(PlayerbotAI* botAI) : ActivityBaseAction(botAI, "coa activity return") {}
    bool Execute(Event event) override;
};

// "coa activity catch up": ActivityMoveFarTo to the leader's position (it is on the bot's map).
class CoaActivityCatchUpAction : public ActivityBaseAction
{
public:
    CoaActivityCatchUpAction(PlayerbotAI* botAI) : ActivityBaseAction(botAI, "coa activity catch up") {}
    bool Execute(Event event) override;
};

// "coa activity celebrate": one text emote chosen by the guid and the activity id (cheer, roar, flex, dance,
// victory... list in the contract), faces the leader or the center; marks the activity celebrated.
class CoaActivityCelebrateAction : public Action
{
public:
    CoaActivityCelebrateAction(PlayerbotAI* botAI) : Action(botAI, "coa activity celebrate") {}
    bool Execute(Event event) override;
};

// "coa activity roam": walks to a point 0.2..0.6 x the leash radius from the center (Jitter), so "grind" finds
// something; nextRoamMs = now + 20 s.
class CoaActivityRoamAction : public ActivityBaseAction
{
public:
    CoaActivityRoamAction(PlayerbotAI* botAI) : ActivityBaseAction(botAI, "coa activity roam") {}
    bool Execute(Event event) override;
};

// "coa challenge duel": re-checks duelTarget (on the map, alive, no duel, within 30 yards), the bot (no group, no
// game-client master) and a real target's rules that can change during the walk (AFK/DND, GM, the duel opt-out, the
// daily cap). The Duel spell reaches 10 yards: farther, the bot walks up to it and keeps duelTarget (the trigger
// fires again until duelApproachUntilMs). Close enough: stamps the cooldowns (ActivityDuelCooldowns::TryStampChallenge;
// false: another bot took it, clear and false), casts spell 7266 on it as RpgDuelAction does ("cast custom spell"
// with the target formatted and " 7266"), then bows at it; challengeSentMs = now. Clears duelTarget.
class CoaChallengeDuelAction : public ActivityBaseAction
{
public:
    CoaChallengeDuelAction(PlayerbotAI* botAI) : ActivityBaseAction(botAI, "coa challenge duel") {}
    bool Execute(Event event) override;
    bool isUseful() override;  // a duelTarget is set and the bot has no duel
};

// "coa duel cancel": withdraws the bot's unanswered challenge (CMSG_DUEL_CANCELLED with its PLAYER_DUEL_ARBITER through
// HandleDuelCancelledOpcode), clears challengeSentMs.
class CoaDuelCancelAction : public Action
{
public:
    CoaDuelCancelAction(PlayerbotAI* botAI) : Action(botAI, "coa duel cancel") {}
    bool Execute(Event event) override;
};

// "coa duel emote": plays pendingEmote facing emoteFace (when on the map and in emote range), clears both.
class CoaDuelEmoteAction : public Action
{
public:
    CoaDuelEmoteAction(PlayerbotAI* botAI) : Action(botAI, "coa duel emote") {}
    bool Execute(Event event) override;
    bool isUseful() override;  // a pendingEmote is set
};

// "coa ring idle": two times out of three a short walk to a point within RING_IDLE_RADIUS of the ring (Jitter), else
// faces a random ring member in sight and plays a friendly emote (the list is in the contract; never a rude one).
// nextIdleMs = now + 10..20 s.
class CoaRingIdleAction : public ActivityBaseAction
{
public:
    CoaRingIdleAction(PlayerbotAI* botAI) : ActivityBaseAction(botAI, "coa ring idle") {}
    bool Execute(Event event) override;
};

// "coa activity attack objective" (combat, M5): puts the objective first in "prioritized targets" and attacks it
// with AttackAction::Attack. M5: also the non-combat "coa activity engage" node's action (the first blow).
class CoaActivityAttackObjectiveAction : public AttackAction
{
public:
    CoaActivityAttackObjectiveAction(PlayerbotAI* botAI) : AttackAction(botAI, "coa activity attack objective") {}
    bool Execute(Event event) override;

    // The bot's own thread. Once `row` (null: the bot left its activity) no longer holds the objective this action
    // put into "prioritized targets" (another objective, none, or ACTIVITY_VIEW_RETREAT), the bot breaks off: that
    // entry goes (a list a command or an RTI set since is kept), "current target" and the melee swing too when they
    // are still on it. Called by "coa activity fall back" and by CoaActivityStrategies::AddNonCombat (every strategy
    // reset, the leave of an activity included).
    static void DropStale(PlayerbotAI* botAI, ActivityMemberView const* row);

private:
    void BreakOff();

    ObjectGuid written;  // the objective this bot put into "prioritized targets" (empty: none)
};

// "coa activity fall back" (combat, M5; non-combat too since the M5 fixes): ACTIVITY_VIEW_RETREAT: clears
// "prioritized targets", stops attacking the former objective (DropStale) and walks to a stable jitter of
// RALLY_ARRIVE_RADIUS around the center (the rally) with ActivityMoveFarTo, fighting or not.
class CoaActivityFallBackAction : public ActivityBaseAction
{
public:
    CoaActivityFallBackAction(PlayerbotAI* botAI) : ActivityBaseAction(botAI, "coa activity fall back") {}
    bool Execute(Event event) override;
    bool isUseful() override;  // the row still has ACTIVITY_VIEW_RETREAT, not in flight
};

#endif
