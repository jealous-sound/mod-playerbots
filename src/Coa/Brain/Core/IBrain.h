/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_IBRAIN_H
#define PLAYERBOTS_IBRAIN_H

#include "BrainTypes.h"
#include <memory>

class BrainSink;

struct BrainTicket
{
    BrainSubmit status = BrainSubmit::Off;
    uint64 jobId = 0;  // set for Queued and Answered
};

// The only face of the brain game code sees. BrainService::Current() returns a NullBrain when the brain is off,
// so callers never test for null. No method blocks on I/O.
class IBrain
{
public:
    virtual ~IBrain() = default;

    // Any thread, relaxed atomics: running, accepting submits, and for ChatReply CoaBrain.Use.ChatReply.
    virtual bool Active(BrainUse use) const = 0;

    // Any thread. Whether an engine of Decision.Chain is Engine.<n>.Calibrated (its probabilities come from the
    // provider). The director asks about messages its heuristic would leave unanswered only then.
    virtual bool ChainCalibrated() const = 0;

    // Any game thread; short locks only. Check-then-commit: a refused job leaves no token, reservation or charge.
    // Queued: exactly one BrainResult reaches `sink` later, unless Cancel() removes the job first or the brain
    // shuts down. Answered (cache hit): `inlineHit`, when not null, holds the result and nothing is posted.
    virtual BrainTicket Submit(BrainJob&& job, std::shared_ptr<BrainSink> const& sink, BrainResult* inlineHit) = 0;

    // Any thread. A job still waiting in its lane is dropped and nothing is posted for it; a job a worker already
    // holds runs to its end and its result is posted (the consumer drops it as stale).
    virtual void Cancel(uint64 jobId) = 0;
};

#endif
