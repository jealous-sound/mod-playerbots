/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RFCMULTIPLIERS_H
#define PLAYERBOTS_RFCMULTIPLIERS_H

#include "Multiplier.h"

class OggleflintMultiplier : public Multiplier
{
    public:
        OggleflintMultiplier(PlayerbotAI* ai) : Multiplier(ai, "oggleflint") {}

    public:
        float GetValue(Action* action) override;
};

class JergoshTheInvokerMultiplier : public Multiplier
{
    public:
        JergoshTheInvokerMultiplier(PlayerbotAI* ai) : Multiplier(ai, "jergosh the invoker") {}

    public:
        float GetValue(Action* action) override;
};

class BazzalanMultiplier : public Multiplier
{
    public:
        BazzalanMultiplier(PlayerbotAI* ai) : Multiplier(ai, "bazzalan") {}

    public:
        float GetValue(Action* action) override;
};

class TaragamanTheHungererMultiplier : public Multiplier
{
    public:
        TaragamanTheHungererMultiplier(PlayerbotAI* ai) : Multiplier(ai, "taragaman the hungerer") {}

    public:
        float GetValue(Action* action) override;
};

#endif
