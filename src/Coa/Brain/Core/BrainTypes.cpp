/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainTypes.h"
#include <ctime>

BrainAnswer const* BrainResult::Find(std::string_view name) const
{
    for (BrainAnswer const& answer : answers)
        if (answer.name == name)
            return &answer;
    return nullptr;
}

char const* BrainLaneName(BrainLane lane)
{
    switch (lane)
    {
        case BrainLane::Player:
            return "player";
        case BrainLane::Organizer:
            return "organizer";
        case BrainLane::Ambient:
            return "ambient";
        default:
            return "unknown";
    }
}

char const* BrainUseName(BrainUse use)
{
    switch (use)
    {
        case BrainUse::ChatReply:
            return "chat-reply";
        case BrainUse::Probe:
            return "probe";
        case BrainUse::Test:
            return "test";
        case BrainUse::Duel:
            return "duel";
        case BrainUse::Organizer:
            return "organizer";
        case BrainUse::Text:
            return "text";
        case BrainUse::Rpg:
            return "rpg";
        case BrainUse::Emote:
            return "emote";
        default:
            return "unknown";
    }
}

char const* BrainSubmitName(BrainSubmit submit)
{
    switch (submit)
    {
        case BrainSubmit::Queued:
            return "queued";
        case BrainSubmit::Answered:
            return "answered";
        case BrainSubmit::Off:
            return "off";
        case BrainSubmit::Invalid:
            return "invalid";
        case BrainSubmit::NoEngine:
            return "no-engine";
        case BrainSubmit::OverBudget:
            return "over-budget";
        case BrainSubmit::Throttled:
            return "throttled";
        case BrainSubmit::TooLate:
            return "too-late";
        case BrainSubmit::Full:
            return "full";
        default:
            return "unknown";
    }
}

char const* BrainErrorName(BrainError error)
{
    switch (error)
    {
        case BrainError::None:
            return "none";
        case BrainError::TooLate:
            return "too-late";
        case BrainError::Cancelled:
            return "cancelled";
        case BrainError::NoEngine:
            return "no-engine";
        case BrainError::Dns:
            return "dns";
        case BrainError::Network:
            return "network";
        case BrainError::Timeout:
            return "timeout";
        case BrainError::ServerError:
            return "server-error";
        case BrainError::BadRequest:
            return "bad-request";
        case BrainError::AuthFailed:
            return "auth-failed";
        case BrainError::Misconfigured:
            return "misconfigured";
        case BrainError::Exhausted:
            return "exhausted";
        case BrainError::BadResponse:
            return "bad-response";
        case BrainError::NotBuilt:
            return "not-built";
        case BrainError::Refused:
            return "refused";
        case BrainError::Rejected:
            return "rejected";
        default:
            return "unknown";
    }
}

char const* BrainJobKindName(BrainJobKind kind)
{
    switch (kind)
    {
        case BrainJobKind::Decision:
            return "decision";
        case BrainJobKind::Text:
            return "text";
        default:
            return "unknown";
    }
}

char const* BrainQuestionTypeName(BrainQuestionType type)
{
    switch (type)
    {
        case BrainQuestionType::Choice:
            return "choice";
        case BrainQuestionType::YesNo:
            return "yes-no";
        case BrainQuestionType::Score:
            return "score";
        default:
            return "unknown";
    }
}

namespace BrainClock
{
namespace
{
int64 FloorDiv(int64 value, int64 divisor)
{
    int64 quotient = value / divisor;
    if ((value % divisor) != 0 && ((value < 0) != (divisor < 0)))
        --quotient;
    return quotient;
}
}  // namespace

// Howard Hinnant's civil_from_days: days since 1970-01-01 to a proleptic Gregorian date.
uint32 UtcDay(int64 unixSeconds)
{
    int64 const z = FloorDiv(unixSeconds, 86400) + 719468;
    int64 const era = (z >= 0 ? z : z - 146096) / 146097;
    int64 const doe = z - era * 146097;
    int64 const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64 year = yoe + era * 400;
    int64 const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64 const mp = (5 * doy + 2) / 153;
    int64 const day = doy - (153 * mp + 2) / 5 + 1;
    int64 const month = mp < 10 ? mp + 3 : mp - 9;
    if (month <= 2)
        ++year;
    if (year < 0)
        return 0;
    return uint32(year * 10000 + month * 100 + day);
}

uint32 UtcDayNow() { return UtcDay(int64(std::time(nullptr))); }

uint32 SecondsToUtcMidnight(int64 unixSeconds)
{
    return uint32(86400 - (unixSeconds - FloorDiv(unixSeconds, 86400) * 86400));
}
}  // namespace BrainClock
