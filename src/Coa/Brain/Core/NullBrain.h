/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NULLBRAIN_H
#define PLAYERBOTS_NULLBRAIN_H

#include "IBrain.h"

// The brain when AiPlayerbot.CoaBrain.Enable = 0, when it failed to start, or after shutdown: every use is
// inactive and every submit is refused with Off. No thread, no socket, no state.
class NullBrain final : public IBrain
{
public:
    bool Active(BrainUse /*use*/) const override { return false; }
    bool ChainCalibrated() const override { return false; }

    BrainTicket Submit(BrainJob&& /*job*/, std::shared_ptr<BrainSink> const& /*sink*/,
                       BrainResult* /*inlineHit*/) override
    {
        return BrainTicket{};
    }

    void Cancel(uint64 /*jobId*/) override {}
};

#endif
