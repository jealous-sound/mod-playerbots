/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYMOVEFARTO_H
#define PLAYERBOTS_ACTIVITYMOVEFARTO_H

#include "ActivityTypes.h"
#include "NewRpgBaseAction.h"
#include <string>

struct ActivityBotState;

// The base of every "coa activity" / "coa duel" action that moves: NewRpgBaseAction's MoveRandomNear and ForceToWait,
// and ActivityMoveFarTo. The bot's map thread.
class ActivityBaseAction : public NewRpgBaseAction
{
public:
    ActivityBaseAction(PlayerbotAI* botAI, std::string const name) : NewRpgBaseAction(botAI, name) {}

protected:
    // A copy of NewRpgBaseAction::MoveFarTo (in flight: true; waiting for the last move; the "keep walking the
    // committed spline" gate; mmap route to the true destination; forward-cone fallback) WITHOUT the stuck teleport:
    // after stuckTime (90 s) without LEADER_PROGRESS_YARDS of progress it only sets `state.stuck` (cleared on
    // progress or a new destination) and keeps trying; the director's straggler rule teleports under the visibility
    // rule. Its stuck bookkeeping lives in ActivityBotState, never in rpgInfo. Cross-map destinations return false
    // (the director teleports). `dest` on the bot's map.
    bool ActivityMoveFarTo(ActivityPoint const& dest);

    // A point around `center` at 0.3..1.0 x `spread` yards, on the ground (Map::GetHeight), not in water; the center
    // itself when none of 4 tries fits.
    ActivityPoint Jitter(ActivityPoint const& center, float spread);

    // The stable jitter every jittered destination goes through: the state's jitterTarget, drawn again (Jitter) only
    // when the center moved more than 1 yard, the phase changed, or the bot came within 3 yards of it. A new target
    // every tick would reset ActivityMoveFarTo's stuck count.
    ActivityPoint StableJitter(ActivityPoint const& center, float spread, ActivityPhase phase);
};

#endif
