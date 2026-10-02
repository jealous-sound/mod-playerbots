/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAACTIVITYTRIGGERS_H
#define PLAYERBOTS_COAACTIVITYTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// Every trigger runs on the bot's map thread; the interval is in milliseconds (Trigger::needCheck). It reads the
// bot's own Player, players on its map, its registry row (CoaActivityBot::Refresh), ActivityDuelCooldowns (its mutex)
// and CoaPlayerPrefs (shared lock), and nothing else of the activity. Each returns false at once when the bot has no
// row (not a participant) unless it says otherwise. Conditions in the contract (section 10.3).

// --- "coa activity" (non-combat) ---

// "coa activity travel", every 1 s. Gathering: the bot is farther than `radius` from the center (any role but a
// leader holding). Marching: the leader, not holding, farther than `radius` from the center.
class CoaActivityTravelTrigger : public Trigger
{
public:
    CoaActivityTravelTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa activity travel", 1 * 1000) {}
    bool IsActive() override;
};

// "coa activity hold", every 1 s: the leader's row has ACTIVITY_VIEW_HOLD.
class CoaActivityHoldTrigger : public Trigger
{
public:
    CoaActivityHoldTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa activity hold", 1 * 1000) {}
    bool IsActive() override;
};

// "coa activity away", every 2 s. Active, a solo participant or the leader, farther than `radius` (the leash) from
// the center on its map.
class CoaActivityAwayTrigger : public Trigger
{
public:
    CoaActivityAwayTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa activity away", 2 * 1000) {}
    bool IsActive() override;
};

// "coa activity catch up", every 2 s. A group member (not the leader) past Gathering, not in combat, whose leader is
// on its map and farther than CATCH_UP_YARDS (another map: the director teleports).
class CoaActivityCatchUpTrigger : public Trigger
{
public:
    CoaActivityCatchUpTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa activity catch up", 2 * 1000) {}
    bool IsActive() override;
};

// "coa activity celebrate", every 1 s: ACTIVITY_VIEW_CELEBRATE and this activity not celebrated by the bot yet.
class CoaActivityCelebrateTrigger : public Trigger
{
public:
    CoaActivityCelebrateTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa activity celebrate", 1 * 1000) {}
    bool IsActive() override;
};

// "coa activity roam", every 5 s. ACTIVITY_VIEW_HUNT, the leader (or a solo hunter), not moving, no "grind target",
// and nextRoamMs passed.
class CoaActivityRoamTrigger : public Trigger
{
public:
    CoaActivityRoamTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa activity roam", 5 * 1000) {}
    bool IsActive() override;
};

// --- "coa duel" (non-combat) ---

// "coa duel partner", checked every 5 s; scans every 20-40 s (nextDuelScanMs). Ring participants with
// ACTIVITY_VIEW_MAY_CHALLENGE, or any bot carrying "coa duel" when ActivityDirector::DuelAmbient() (no row needed).
// Picks one partner by the contract's rules and preference order and stores it in duelTarget; while the bot walks
// within the Duel spell's 10 yards of it (at most 15 s) the trigger stays active without a new scan.
class CoaDuelPartnerTrigger : public Trigger
{
public:
    CoaDuelPartnerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa duel partner", 5 * 1000) {}
    bool IsActive() override;
};

// "coa duel ended", every 1 s (no row needed: ambient bots too). The bot's own outcome
// (ActivityDuelCooldowns::TakeOutcome): Won -> cheer, Lost -> bow at the winner (a third of the time cry), Fled ->
// no emote. Or, as a ring spectator, a newer ringDuelEndMs than seenRingDuelEndMs: half the time applaud. Stores
// pendingEmote and emoteFace.
class CoaDuelEndedTrigger : public Trigger
{
public:
    CoaDuelEndedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa duel ended", 1 * 1000) {}
    bool IsActive() override;
};

// "coa duel unanswered", every 5 s (no row needed): the bot's own challenge (it is the initiator, still
// DUEL_STATE_CHALLENGED) has waited CHALLENGE_TIMEOUT_MS since challengeSentMs.
class CoaDuelUnansweredTrigger : public Trigger
{
public:
    CoaDuelUnansweredTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa duel unanswered", 5 * 1000) {}
    bool IsActive() override;
};

// "coa ring idle", every 3 s. A ring participant in Active, not in combat, no duel, within the leash, nextIdleMs
// passed (then 10-20 s later).
class CoaRingIdleTrigger : public Trigger
{
public:
    CoaRingIdleTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa ring idle", 3 * 1000) {}
    bool IsActive() override;
};

// "coa activity engage" (M5), every 1 s, out of combat: the row has an objective and no ACTIVITY_VIEW_RETREAT, the
// objective is in reach (CoaActivityBot::ObjectiveInReach) and the bot is alive. Starts the fight (hunts, raids).
class CoaActivityEngageTrigger : public Trigger
{
public:
    CoaActivityEngageTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa activity engage", 1 * 1000) {}
    bool IsActive() override;
};

// --- "coa activity combat" ---

// "coa activity objective", every 1 s. The row has an objective (M5), alive, attackable, on the bot's map within 60
// yards, and it is not the bot's current target. M5: never with ACTIVITY_VIEW_RETREAT.
class CoaActivityObjectiveTrigger : public Trigger
{
public:
    CoaActivityObjectiveTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa activity objective", 1 * 1000) {}
    bool IsActive() override;
};

// "coa activity retreat" (M5), every 1 s, in combat: the row has ACTIVITY_VIEW_RETREAT, its center is set on the bot's
// map and the bot is farther than RALLY_ARRIVE_RADIUS from it.
class CoaActivityRetreatTrigger : public Trigger
{
public:
    CoaActivityRetreatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "coa activity retreat", 1 * 1000) {}
    bool IsActive() override;
};

#endif
