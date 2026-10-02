/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MEMORYEVENT_H
#define PLAYERBOTS_MEMORYEVENT_H

#include "Define.h"
#include <cstddef>
#include <string>

// The kinds of diary events. The numbers are stored in playerbots_coa_memory_event.kind: never renumber, only
// append. "Them" is the event's other character; the bot is the diary's owner.
enum class MemoryEventKind : uint8
{
    None = 0,
    Met = 1,             // first meeting, or the first after a day apart
    ChatFriendly = 2,    // a friendly message (merged: one per pair per 10 minutes)
    ChatHostile = 3,     // a hostile message (merged likewise)
    Thanked = 4,         // they thanked the bot (merged likewise)
    Insulted = 5,        // they insulted the bot (merged likewise)
    EmoteFriendly = 6,   // a friendly text emote (merged likewise)
    EmoteHostile = 7,    // a rude text emote (merged likewise)
    DuelWon = 8,         // the bot won a duel against them
    DuelLost = 9,        // the bot lost a duel to them (detail "fled" when the bot fled)
    DuelFled = 10,       // they fled a duel with the bot
    KilledBy = 11,       // they killed the bot (outside battlegrounds, or everywhere with BattlegroundKills = 2)
    Killed = 12,         // the bot killed them (likewise)
    Helped = 13,         // they killed a creature the bot had tagged
    HelpedThem = 14,     // the bot killed a creature they had tagged
    Grouped = 15,        // they and the bot joined one group (once per pair per 30 minutes)
    KickedBy = 16,       // they kicked the bot from a group
    Traded = 17,         // a completed trade
    BossKill = 18,       // a dungeon or world boss killed together
    LeaderKill = 19,     // a CoaMemory.LeaderEntries creature killed together
    Count
};

constexpr std::size_t MEMORY_DETAIL_MAX = 160;  // bytes, the detail column
constexpr uint8 MEMORY_IMPORTANCE_MAX = 10;
constexpr uint8 MEMORY_IMPORTANCE_REAL_BONUS = 2;  // added when the other character is a real player

// "met", "chat-friendly", "chat-hostile", "thanked", "insulted", "emote-friendly", "emote-hostile", "duel-won",
// "duel-lost", "duel-fled", "killed-by", "killed", "helped", "helped-them", "grouped", "kicked-by", "traded",
// "boss-kill", "leader-kill"; "none" otherwise.
char const* MemoryEventKindName(MemoryEventKind kind);

// Base importance (1..10) before the real-player bonus: Met 1; the chat and emote kinds 2; HelpedThem 2; Helped,
// Grouped, Traded 3; DuelWon, DuelLost, Thanked, Insulted 4; DuelFled, Killed, KickedBy 5; KilledBy, BossKill 6;
// LeaderKill 9.
uint8 MemoryEventBaseImportance(MemoryEventKind kind);

// The kinds merged per pair within 10 minutes: ChatFriendly, ChatHostile, Thanked, Insulted, EmoteFriendly,
// EmoteHostile.
bool MemoryEventMerges(MemoryEventKind kind);

// One diary line. Built by code: `detail` holds fixed English words and numbers only, never a player's words and
// never a name (the other character is other_guid).
struct MemoryEvent
{
    uint64 id = 0;            // the row id when loaded; 0 for an event made in this session
    uint32 otherGuid = 0;     // 0: about the bot itself
    uint32 otherAccount = 0;  // 0 for bots
    MemoryEventKind kind = MemoryEventKind::None;
    uint8 importance = 0;     // 1..10
    uint16 mapId = 0;
    uint32 zoneId = 0;
    uint32 ts = 0;            // unix time
    std::string detail;       // at most MEMORY_DETAIL_MAX bytes, ASCII
};

#endif
