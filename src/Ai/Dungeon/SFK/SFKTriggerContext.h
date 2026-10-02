/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SFKTRIGGERCONTEXT_H
#define PLAYERBOTS_SFKTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "SFKTriggers.h"

class VanillaDungeonSFKTriggerContext : public NamedObjectContext<Trigger>
{
    public:
        VanillaDungeonSFKTriggerContext()
        {
            creators["nandos pack"] = &VanillaDungeonSFKTriggerContext::nandos_pack;
            creators["arugal thundershock"] = &VanillaDungeonSFKTriggerContext::arugal_thundershock;
        }
    private:
        static Trigger* nandos_pack(PlayerbotAI* ai) { return new NandosPackTrigger(ai); }
        static Trigger* arugal_thundershock(PlayerbotAI* ai) { return new ArugalThundershockTrigger(ai); }
};

#endif
