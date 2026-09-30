/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityTypes.h"
#include "Util.h"

char const* ActivityKindName(ActivityKindId kind)
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return "duel";
        case ActivityKindId::Gang:
            return "gang";
        case ActivityKindId::Dungeon:
            return "dungeon";
        case ActivityKindId::Hunt:
            return "hunt";
        case ActivityKindId::Raid:
            return "raid";
        case ActivityKindId::Party:
            return "party";
        case ActivityKindId::Loiter:
            return "loiter";
        case ActivityKindId::Gurubashi:
            return "gurubashi";
        default:
            return "none";
    }
}

std::optional<ActivityKindId> ActivityKindFromName(std::string_view name)
{
    for (uint8 k = uint8(ActivityKindId::None) + 1; k < uint8(ActivityKindId::Count); ++k)
        if (StringEqualI(name, ActivityKindName(ActivityKindId(k))))
            return ActivityKindId(k);
    return std::nullopt;
}

char const* ActivityPhaseName(ActivityPhase phase)
{
    switch (phase)
    {
        case ActivityPhase::Planning:
            return "planning";
        case ActivityPhase::Recruiting:
            return "recruiting";
        case ActivityPhase::Forming:
            return "forming";
        case ActivityPhase::Gathering:
            return "gathering";
        case ActivityPhase::Marching:
            return "marching";
        case ActivityPhase::Active:
            return "active";
        case ActivityPhase::Resolving:
            return "resolving";
        case ActivityPhase::Disbanding:
            return "disbanding";
        default:
            return "unknown";
    }
}

char const* ActivityOutcomeName(ActivityOutcome outcome)
{
    switch (outcome)
    {
        case ActivityOutcome::None:
            return "none";
        case ActivityOutcome::Success:
            return "success";
        case ActivityOutcome::Timeout:
            return "timeout";
        case ActivityOutcome::TooFew:
            return "too-few";
        case ActivityOutcome::LeaderGone:
            return "leader-gone";
        case ActivityOutcome::LoadAbort:
            return "load";
        case ActivityOutcome::Stopped:
            return "stopped";
        case ActivityOutcome::Disabled:
            return "disabled";
        case ActivityOutcome::Shutdown:
            return "shutdown";
        case ActivityOutcome::Restart:
            return "restart";
        case ActivityOutcome::NoPlayers:
            return "no-players";
        case ActivityOutcome::Guarded:
            return "guarded";
        default:
            return "unknown";
    }
}

char const* ActivityLeaveReasonName(ActivityLeaveReason reason)
{
    switch (reason)
    {
        case ActivityLeaveReason::None:
            return "none";
        case ActivityLeaveReason::Gone:
            return "gone";
        case ActivityLeaveReason::Dropped:
            return "dropped";
        case ActivityLeaveReason::Grouped:
            return "grouped";
        case ActivityLeaveReason::Left:
            return "left";
        case ActivityLeaveReason::Kicked:
            return "kicked";
        case ActivityLeaveReason::Ended:
            return "ended";
        default:
            return "unknown";
    }
}
