/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MEMORYRETRIEVAL_H
#define PLAYERBOTS_MEMORYRETRIEVAL_H

#include "Define.h"
#include "MemoryEvent.h"
#include "MemoryTypes.h"
#include <string>
#include <string_view>
#include <vector>

class BotMemory;
struct MemoryRelation;

// Memory rendered as short text: compact English lines for the brain state (features only: never a name, the
// speaker is "the speaker"), and lines with names for GM commands. World thread (it reads BotMemory).
namespace MemoryRetrieval
{
// The kind of decision the lines serve; events of the matching kinds get relevance 0.4.
enum class Context : uint8
{
    Chat = 0,  // chat, thanks, insult and emote kinds
    Duel,      // duel kinds
    Pvp,       // KilledBy, Killed
    Group      // Grouped, KickedBy, BossKill, LeaderKill, Helped, HelpedThem, Traded
};

// Events of `memory` best first, at most `max`. Score = 0.5 * importance / 10 + 0.3 * exp(-ageHours / 72)
// + 0.2 * relevance, relevance 1 when the event is about `aboutGuid`, else 0.6 in `zoneId`, else 0.4 when it
// matches `context`, else 0. Ties: newer first. `aboutOnly`: only the events about `aboutGuid` are ranked (filtered
// before the cut). Pointers into memory.events: valid until memory changes.
std::vector<MemoryEvent const*> Rank(BotMemory const& memory, uint32 aboutGuid, uint32 zoneId, Context context,
                                     uint32 now, std::size_t max, bool aboutOnly = false);

// Brain lines, features only. `label` is the bot's ballot name ("b0").
//   "b0 and the speaker: grudge; affinity -35, trust 2, grudge 42, fear 10, familiarity 12; met 3 times, last 2 h
//   ago; chats 5; killed b0 2 times; b0 won 1 duels, lost 3"
// Axes in points (value / 10, truncated), only the non-zero ones, in MemoryAxis order, decayed to `now`; counters
// only when not zero, in MemoryCounter order, worded as in the contract.
std::string BrainRelationLine(std::string_view label, MemoryRelation const& relation, MemoryDecay const& decay,
                              uint32 now);
// "b0 [2d ago] the speaker killed b0 in Hillsbrad Foothills" (phrases per kind in the contract; the zone from the
// English DBC area name, left out when unknown). Only events about the speaker are given to the brain.
std::string BrainEventLine(std::string_view label, MemoryEvent const& event, uint32 now);

// GM lines (names allowed).
//   "Kaelis Dawnfire (player, account 12): friend; affinity 42, trust 30 ...; met 12, chats 40 ...; seen 2 h ago"
std::string ShowRelationLine(MemoryRelation const& relation, MemoryDecay const& decay, uint32 now);
//   "2026-09-28 21:14 UTC killed-by Kaelis Dawnfire (6) Hillsbrad Foothills: level 23 vs 30"
std::string ShowEventLine(MemoryEvent const& event, std::string const& otherName, uint32 now);

// "5 min", "2 h", "3 d" (floor; "now" under a minute).
std::string AgeText(uint32 seconds);
// Rough token count of text: bytes / 4, rounded up.
uint32 EstimateTokens(std::string_view text);
}  // namespace MemoryRetrieval

#endif
