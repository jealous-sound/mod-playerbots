/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SFKActions.h"
#include "AiObjectContext.h"
#include "Playerbots.h"

bool AttackNandosPackAction::isUseful() { return !botAI->IsHeal(bot); }
bool AttackNandosPackAction::Execute(Event /*event*/)
{
    Unit* add = NandosPack::Nearest(botAI, bot, context);
    if (!add || AI_VALUE(Unit*, "current target") == add)
        return false;

    return Attack(add);
}

bool ArugalAvoidThundershockAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "archmage arugal");
    if (!boss)
        return false;

    float const distance = bot->GetExactDist2d(boss->GetPosition());
    float const wanted = THUNDERSHOCK_RADIUS + THUNDERSHOCK_SAFETY;
    if (distance >= wanted)
        return false;

    return MoveAway(boss, wanted - distance);
}
