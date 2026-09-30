/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYBRAIN_H
#define PLAYERBOTS_ACTIVITYBRAIN_H

#include "ActivityTypes.h"
#include "BrainTypes.h"
#include "Define.h"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// The activity's questions to the brain, built from plain features (never a name, never a player's words) and read
// back into plain answers. Pure functions: the builders run on the world thread (Organizer) or on the bot's map
// thread (duel accept), the readers on the thread that drains the sink. English wording (contract section 11).
namespace ActivityBrain
{
constexpr char const* SITUATION_NEXT = "activity_next";
constexpr char const* SITUATION_JOIN = "activity_join";
constexpr char const* SITUATION_DUEL = "duel_accept";

// The realm as the scheduler sees it.
struct Scene
{
    uint32 realPlayers = 0;
    std::vector<uint8> playerLevels;  // online real players' levels, at most 8 listed
    uint32 freeBots = 0;
    uint32 participants = 0;
    uint32 running[uint8(ActivityKindId::Count)] = {};  // live activities per kind
    uint32 hourOfDay = 0;                               // server local hour
};

// One candidate of a join question, "b<i>" in the state.
struct Candidate
{
    uint8 level = 0;
    uint8 classId = 0;
    uint32 bravery = 0;      // 0..99
    uint32 sociability = 0;  // 0..99
    uint32 pvpAppetite = 0;  // 0..99
    int32 distance = -1;     // yards to the rally, -1 far away (another map)
};

struct Choice
{
    bool valid = false;  // a usable answer came back
    bool none = false;   // the brain chose "none": nothing new this time
    ActivityKindId kind = ActivityKindId::None;
};

// Organizer lane, BrainUse::Organizer, not cached. One Choice question "next" whose options are `eligible` (their
// ActivityKindName) plus "none". tag: the director's schedule id.
BrainJob BuildNextKindJob(Scene const& scene, std::vector<ActivityKindId> const& eligible, uint64 tag,
                          uint64 deadlineMs);
Choice ReadNextKind(BrainResult const& result, std::vector<ActivityKindId> const& eligible);

// Organizer lane, BrainUse::Organizer, not cached. One YesNo question "join_<i>" per candidate (at most
// MAX_JOIN_QUESTIONS; the caller asks for the first ones only). `where`: the zone or spot in English words.
BrainJob BuildJoinJob(ActivityKindId kind, Scene const& scene, std::string const& where, uint8 minLevel,
                      uint8 maxLevel, std::vector<Candidate> const& candidates, uint64 tag, uint64 deadlineMs);
// Per candidate: 1 yes (p >= 0.5), 0 no, -1 no valid answer (the dice decide). Always `count` entries.
std::vector<int8> ReadJoin(BrainResult const& result, std::size_t count);

// The duel accept (a real challenger). Player lane (a player waits), BrainUse::Duel, not cached.
struct DuelFeatures
{
    uint8 botLevel = 0;
    uint8 challengerLevel = 0;
    uint8 botHealthPct = 0;
    uint8 botClass = 0;
    uint8 challengerClass = 0;
    bool rival = false;          // the bot's memory: label Rival or Grudge (RivalDuels on)
    bool friendly = false;       // label Friend
    uint32 duelsToday = 0;       // duels this account fought against bots today
    uint32 bravery = 0;          // 0..99
    bool inRing = false;         // the bot is at a duel ring
};
BrainJob BuildDuelJob(DuelFeatures const& features, uint64 botGuidRaw, uint64 tag, uint64 deadlineMs);
// nullopt: no valid "duel_accept" answer.
std::optional<bool> ReadDuel(BrainResult const& result, float threshold);
}  // namespace ActivityBrain

#endif
