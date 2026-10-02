/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SFKACTIONCONTEXT_H
#define PLAYERBOTS_SFKACTIONCONTEXT_H

#include "Action.h"
#include "MovementActions.h"
#include "NamedObjectContext.h"
#include "SFKActions.h"

class VanillaDungeonSFKActionContext : public NamedObjectContext<Action>
{
    public:
        VanillaDungeonSFKActionContext() {
            creators["attack nandos pack"] = &VanillaDungeonSFKActionContext::attack_nandos_pack;
            creators["avoid thundershock"] = &VanillaDungeonSFKActionContext::avoid_thundershock;
        }
    private:
        static Action* attack_nandos_pack(PlayerbotAI* ai) { return new AttackNandosPackAction(ai); }
        static Action* avoid_thundershock(PlayerbotAI* ai) { return new ArugalAvoidThundershockAction(ai); }
};

#endif
