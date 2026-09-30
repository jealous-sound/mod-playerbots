/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYORGANIZER_H
#define PLAYERBOTS_ACTIVITYORGANIZER_H

#include "BrainTypes.h"
#include "Define.h"
#include <map>
#include <memory>
#include <optional>
#include <utility>

class BrainSink;
struct ActivityConfig;

// The questions a live activity asks the brain on the Organizer lane (M5). At most one of each per activity is out.
enum class ActivityAsk : uint8
{
    LineKey = 0,   // which recruitment text name the next post uses
    Retreat,       // should the raid fall back now
    ClosingEmote,  // the emote every bot plays at the end
    Count
};

// Where an activity's question stands.
enum class ActivityAskState : uint8
{
    None = 0,  // never asked, or already taken
    Pending,   // out, before its deadline
    Ready,     // answered, or its deadline passed (Take then gives nullopt: the kind's dice decide)
};

// The kinds' own brain questions (M5), on the director's Organizer sink. World thread only, inside the director's
// tick (COA_ASSERT_WORLD_THREAD). A member of the director, handed to the kinds through ActivityServices::organizer.
// Tags come from their own range (ORGANIZER_TAG_BASE and up), so the director tells them from the schedule's and the
// plans' tags. Nothing here waits: a question is asked on one tick and read on a later one, and every question has
// a dice fallback in the kind.
class ActivityOrganizer
{
public:
    static constexpr uint64 ORGANIZER_TAG_BASE = uint64(1) << 40;

    // The director, at the start of each tick (before the kinds): the config snapshot of this tick, the sink the
    // jobs answer into (the director's organizerSink; may be null before the first start), and the time.
    void BeginTick(ActivityConfig const* config, std::shared_ptr<BrainSink> sink, uint64 nowMs);

    // Use.Organizer is on and the brain takes Organizer jobs now (BrainService::Current()->Active(Organizer)).
    bool Usable() const;

    // Submits `job` (built by an ActivityBrain::Build*Job with any tag and deadline: both are replaced here, the
    // deadline by now + BrainDeadlineMs) for (activityId, slot) unless one is out already. False when the brain is
    // not usable or refused the job: the kind decides with its dice at once. True: Queued, read it with State/Take.
    bool Ask(uint32 activityId, ActivityAsk slot, BrainJob job);

    ActivityAskState State(uint32 activityId, ActivityAsk slot) const;

    // Ready: the answer (nullopt when the deadline passed first, the job is cancelled then; or when the result is an
    // error). Forgets the question. None or Pending: nullopt, nothing changes.
    std::optional<BrainResult> Take(uint32 activityId, ActivityAsk slot);

    // The director, for every drained Organizer result whose tag is neither the schedule's nor a plan's. True when it
    // answered one of these questions (kept for Take); false: stale, dropped by the caller.
    bool Deliver(BrainResult const& result);

    // The director: an activity ended (its questions cancelled and forgotten); StopRunning/Shutdown (all).
    void Forget(uint32 activityId);
    void Clear();

private:
    struct Question
    {
        uint64 tag = 0;
        uint64 jobId = 0;
        uint64 deadlineMs = 0;
        bool answered = false;
        BrainResult result;
    };

    using Key = std::pair<uint32, uint8>;  // activity id, ActivityAsk

    ActivityConfig const* config = nullptr;
    std::shared_ptr<BrainSink> sink;
    uint64 nowMs = 0;
    uint64 nextTag = ORGANIZER_TAG_BASE;
    std::map<Key, Question> questions;
    std::map<uint64, Key> byTag;
};

#endif
