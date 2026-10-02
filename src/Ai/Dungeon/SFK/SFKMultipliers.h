/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SFKMULTIPLIERS_H
#define PLAYERBOTS_SFKMULTIPLIERS_H

#include "Multiplier.h"

class WolfMasterNandosMultiplier : public Multiplier
{
    public:
        WolfMasterNandosMultiplier(PlayerbotAI* ai) : Multiplier(ai, "wolf master nandos") {}

    public:
        float GetValue(Action* action) override;
};

class ArchmageArugalMultiplier : public Multiplier
{
    public:
        ArchmageArugalMultiplier(PlayerbotAI* ai) : Multiplier(ai, "archmage arugal") {}

    public:
        float GetValue(Action* action) override;
};

#endif
