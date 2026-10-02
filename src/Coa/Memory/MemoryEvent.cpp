/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MemoryEvent.h"

char const* MemoryEventKindName(MemoryEventKind kind)
{
    switch (kind)
    {
        case MemoryEventKind::Met:
            return "met";
        case MemoryEventKind::ChatFriendly:
            return "chat-friendly";
        case MemoryEventKind::ChatHostile:
            return "chat-hostile";
        case MemoryEventKind::Thanked:
            return "thanked";
        case MemoryEventKind::Insulted:
            return "insulted";
        case MemoryEventKind::EmoteFriendly:
            return "emote-friendly";
        case MemoryEventKind::EmoteHostile:
            return "emote-hostile";
        case MemoryEventKind::DuelWon:
            return "duel-won";
        case MemoryEventKind::DuelLost:
            return "duel-lost";
        case MemoryEventKind::DuelFled:
            return "duel-fled";
        case MemoryEventKind::KilledBy:
            return "killed-by";
        case MemoryEventKind::Killed:
            return "killed";
        case MemoryEventKind::Helped:
            return "helped";
        case MemoryEventKind::HelpedThem:
            return "helped-them";
        case MemoryEventKind::Grouped:
            return "grouped";
        case MemoryEventKind::KickedBy:
            return "kicked-by";
        case MemoryEventKind::Traded:
            return "traded";
        case MemoryEventKind::BossKill:
            return "boss-kill";
        case MemoryEventKind::LeaderKill:
            return "leader-kill";
        default:
            return "none";
    }
}

uint8 MemoryEventBaseImportance(MemoryEventKind kind)
{
    switch (kind)
    {
        case MemoryEventKind::Met:
            return 1;
        case MemoryEventKind::ChatFriendly:
        case MemoryEventKind::ChatHostile:
        case MemoryEventKind::EmoteFriendly:
        case MemoryEventKind::EmoteHostile:
        case MemoryEventKind::HelpedThem:
            return 2;
        case MemoryEventKind::Helped:
        case MemoryEventKind::Grouped:
        case MemoryEventKind::Traded:
            return 3;
        case MemoryEventKind::DuelWon:
        case MemoryEventKind::DuelLost:
        case MemoryEventKind::Thanked:
        case MemoryEventKind::Insulted:
            return 4;
        case MemoryEventKind::DuelFled:
        case MemoryEventKind::Killed:
        case MemoryEventKind::KickedBy:
            return 5;
        case MemoryEventKind::KilledBy:
        case MemoryEventKind::BossKill:
            return 6;
        case MemoryEventKind::LeaderKill:
            return 9;
        default:
            return 0;
    }
}

bool MemoryEventMerges(MemoryEventKind kind)
{
    switch (kind)
    {
        case MemoryEventKind::ChatFriendly:
        case MemoryEventKind::ChatHostile:
        case MemoryEventKind::Thanked:
        case MemoryEventKind::Insulted:
        case MemoryEventKind::EmoteFriendly:
        case MemoryEventKind::EmoteHostile:
            return true;
        default:
            return false;
    }
}
