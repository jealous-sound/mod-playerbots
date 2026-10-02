/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SFKACTIONS_H
#define PLAYERBOTS_SFKACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "MovementActions.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SFKTriggers.h"

class AttackNandosPackAction : public AttackAction
{
public:
    AttackNandosPackAction(PlayerbotAI* ai) : AttackAction(ai, "attack nandos pack") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class ArugalAvoidThundershockAction : public MovementAction
{
public:
    ArugalAvoidThundershockAction(PlayerbotAI* ai) : MovementAction(ai, "avoid thundershock") {}
    bool Execute(Event event) override;
};

#endif
