/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SFKMultipliers.h"
#include "AiObjectContext.h"
#include "ChooseTargetActions.h"
#include "GenericSpellActions.h"
#include "Playerbots.h"
#include "SFKActions.h"
#include "SFKTriggers.h"

namespace
{
    // Without this the ordinary "attack what the tank attacks" behaviour drags every bot straight
    // back onto Nandos the moment the priority action has switched it to a wolf.
    bool IsAssist(Action* action) { return dynamic_cast<DpsAssistAction*>(action) != nullptr; }
}

float WolfMasterNandosMultiplier::GetValue(Action* action)
{
    if (!IsAssist(action))
        return 1.0f;

    return NandosPack::Nearest(botAI, bot, context) ? 0.0f : 1.0f;
}

float ArchmageArugalMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "archmage arugal");
    if (!boss || !botAI->IsRanged(bot))
        return 1.0f;

    // Shadow Port keeps putting him down next to the ranged. Until they are clear of Thundershock
    // the formation and follow actions must not walk them back in.
    if (bot->GetExactDist2d(boss->GetPosition()) < THUNDERSHOCK_RADIUS + THUNDERSHOCK_SAFETY)
    {
        if (dynamic_cast<MovementAction*>(action) && !dynamic_cast<ArugalAvoidThundershockAction*>(action))
            return 0.0f;
    }

    return 1.0f;
}
