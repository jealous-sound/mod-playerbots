/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MemoryTypes.h"

char const* MemoryAxisName(MemoryAxis axis)
{
    switch (axis)
    {
        case MemoryAxis::Affinity:
            return "affinity";
        case MemoryAxis::Trust:
            return "trust";
        case MemoryAxis::Respect:
            return "respect";
        case MemoryAxis::Rivalry:
            return "rivalry";
        case MemoryAxis::Gratitude:
            return "gratitude";
        case MemoryAxis::Grudge:
            return "grudge";
        case MemoryAxis::Fear:
            return "fear";
        case MemoryAxis::Annoyance:
            return "annoyance";
        case MemoryAxis::Familiarity:
            return "familiarity";
        case MemoryAxis::Camaraderie:
            return "camaraderie";
        default:
            return "none";
    }
}

char const* MemoryCounterName(MemoryCounter counter)
{
    switch (counter)
    {
        case MemoryCounter::Meetings:
            return "meetings";
        case MemoryCounter::Chats:
            return "chats";
        case MemoryCounter::HelpedMe:
            return "helped_me";
        case MemoryCounter::HelpedThem:
            return "helped_them";
        case MemoryCounter::KilledMe:
            return "killed_me";
        case MemoryCounter::KilledThem:
            return "killed_them";
        case MemoryCounter::DuelsWon:
            return "duels_won";
        case MemoryCounter::DuelsLost:
            return "duels_lost";
        case MemoryCounter::GroupsShared:
            return "groups_shared";
        case MemoryCounter::Trades:
            return "trades";
        default:
            return "none";
    }
}

char const* MemoryLabelName(MemoryLabel label)
{
    switch (label)
    {
        case MemoryLabel::Acquaintance:
            return "acquaintance";
        case MemoryLabel::Friend:
            return "friend";
        case MemoryLabel::Rival:
            return "rival";
        case MemoryLabel::Disliked:
            return "disliked";
        case MemoryLabel::Grudge:
            return "grudge";
        default:
            return "stranger";
    }
}

char const* MemoryTalkKindName(MemoryTalkKind kind)
{
    switch (kind)
    {
        case MemoryTalkKind::Whisper:
            return "whisper";
        case MemoryTalkKind::Group:
            return "group";
        case MemoryTalkKind::Guild:
            return "guild";
        case MemoryTalkKind::Say:
            return "say";
        case MemoryTalkKind::Channel:
            return "channel";
        case MemoryTalkKind::Emote:
            return "emote";
    }
    return "say";
}
