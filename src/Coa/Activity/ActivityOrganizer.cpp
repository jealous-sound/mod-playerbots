/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityOrganizer.h"
#include "ActivityConfig.h"
#include "BrainService.h"
#include "BrainSink.h"
#include "CoaThread.h"
#include "Log.h"
#include <utility>

namespace
{
constexpr char const* LOGGER = "playerbots.activity";

char const* AskName(uint8 slot)
{
    switch (ActivityAsk(slot))
    {
        case ActivityAsk::LineKey:
            return "line";
        case ActivityAsk::Retreat:
            return "retreat";
        case ActivityAsk::ClosingEmote:
            return "emote";
        default:
            return "?";
    }
}
}  // namespace

void ActivityOrganizer::BeginTick(ActivityConfig const* tickConfig, std::shared_ptr<BrainSink> tickSink, uint64 now)
{
    COA_ASSERT_WORLD_THREAD();
    config = tickConfig;
    sink = std::move(tickSink);
    nowMs = now;
}

bool ActivityOrganizer::Usable() const
{
    return config && config->useBrainOrganizer && sink && BrainService::Current()->Active(BrainUse::Organizer);
}

bool ActivityOrganizer::Ask(uint32 activityId, ActivityAsk slot, BrainJob job)
{
    COA_ASSERT_WORLD_THREAD();
    Key const key{activityId, uint8(slot)};
    if (!Usable() || questions.count(key))
        return false;

    uint64 const tag = nextTag++;
    uint64 const deadline = nowMs + config->brainDeadlineMs;
    job.tag = tag;
    job.deadlineMs = deadline;
    job.use = BrainUse::Organizer;
    job.lane = BrainLane::Organizer;
    job.cacheKey.clear();

    BrainTicket const ticket = BrainService::Current()->Submit(std::move(job), sink, nullptr);
    if (ticket.status != BrainSubmit::Queued)
    {
        LOG_DEBUG(LOGGER, "activity #{}: {} question not asked ({})", activityId, AskName(uint8(slot)),
                  BrainSubmitName(ticket.status));
        return false;
    }

    Question& question = questions[key];
    question.tag = tag;
    question.jobId = ticket.jobId;
    question.deadlineMs = deadline;
    byTag[tag] = key;
    return true;
}

ActivityAskState ActivityOrganizer::State(uint32 activityId, ActivityAsk slot) const
{
    auto const it = questions.find(Key{activityId, uint8(slot)});
    if (it == questions.end())
        return ActivityAskState::None;
    if (it->second.answered || nowMs >= it->second.deadlineMs)
        return ActivityAskState::Ready;
    return ActivityAskState::Pending;
}

std::optional<BrainResult> ActivityOrganizer::Take(uint32 activityId, ActivityAsk slot)
{
    COA_ASSERT_WORLD_THREAD();
    if (State(activityId, slot) != ActivityAskState::Ready)
        return std::nullopt;

    auto const it = questions.find(Key{activityId, uint8(slot)});
    Question question = std::move(it->second);
    questions.erase(it);
    byTag.erase(question.tag);

    if (!question.answered)
    {
        // Past its deadline: the kind's dice decide; a late answer finds no question and is dropped.
        BrainService::Current()->Cancel(question.jobId);
        LOG_DEBUG(LOGGER, "activity #{}: {} question late, dice", activityId, AskName(uint8(slot)));
        return std::nullopt;
    }
    if (question.result.error != BrainError::None)
    {
        LOG_DEBUG(LOGGER, "activity #{}: {} question failed ({}), dice", activityId, AskName(uint8(slot)),
                  BrainErrorName(question.result.error));
        return std::nullopt;
    }
    return std::move(question.result);
}

bool ActivityOrganizer::Deliver(BrainResult const& result)
{
    COA_ASSERT_WORLD_THREAD();
    auto const tagged = byTag.find(result.tag);
    if (tagged == byTag.end())
        return false;
    auto const it = questions.find(tagged->second);
    if (it == questions.end() || it->second.answered)
        return false;
    it->second.result = result;
    it->second.answered = true;
    return true;
}

void ActivityOrganizer::Forget(uint32 activityId)
{
    COA_ASSERT_WORLD_THREAD();
    auto it = questions.lower_bound(Key{activityId, 0});
    while (it != questions.end() && it->first.first == activityId)
    {
        if (!it->second.answered)
            BrainService::Current()->Cancel(it->second.jobId);
        byTag.erase(it->second.tag);
        it = questions.erase(it);
    }
}

void ActivityOrganizer::Clear()
{
    COA_ASSERT_WORLD_THREAD();
    if (!questions.empty())
    {
        std::shared_ptr<IBrain> const brain = BrainService::Current();
        for (auto const& [key, question] : questions)
            if (!question.answered)
                brain->Cancel(question.jobId);
    }
    questions.clear();
    byTag.clear();
}
