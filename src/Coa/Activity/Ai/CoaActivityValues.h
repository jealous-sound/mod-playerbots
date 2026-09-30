/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAACTIVITYVALUES_H
#define PLAYERBOTS_COAACTIVITYVALUES_H

#include "ActivityRegistry.h"
#include "ActivityTypes.h"
#include "ObjectGuid.h"
#include "Value.h"
#include <memory>

class Player;
class PlayerbotAI;
class Unit;

// Per-bot state of the "coa activity", "coa activity combat" and "coa duel" strategies. Touched only by the bot's own
// AI objects on its map thread; never saved, so it restarts empty at each login.
struct ActivityBotState
{
    // The bot's registry row, re-read only when ActivityRegistry::Version() changed (CoaActivityBot::Refresh).
    uint64 version = 0;
    std::shared_ptr<ActivityRegistrySnapshot const> snapshot;  // keeps `row` alive
    ActivityMemberView const* row = nullptr;  // null: not a participant (or a row of another life of the bot)

    // ActivityMoveFarTo's bookkeeping (not NewRpgInfo's: the activity never shares NewRpg's stuck state).
    ActivityPoint moveFarDest;
    float nearestMoveFarDis = 0.0f;
    uint32 stuckTs = 0;           // getMSTime() of the last real progress
    uint32 stuckAttempts = 0;
    bool stuck = false;           // no progress for 90 s: the director's straggler rule takes over (no teleport here)
    // A jittered destination stays the same while its center does, so ActivityMoveFarTo's stuck count means
    // something: redrawn when the center moves (> 1 yard), the phase changes or the point is reached.
    ActivityPoint jitterFor;      // the center `jitterTarget` was drawn around
    ActivityPoint jitterTarget;
    ActivityPhase jitterPhase = ActivityPhase::Planning;  // the phase it was drawn in

    // Duels.
    ObjectGuid duelTarget;        // set by "coa duel partner" for "coa challenge duel"
    uint64 duelApproachUntilMs = 0;  // the Duel spell reaches 10 yards: the bot walks to duelTarget until then
    uint64 challengeSentMs = 0;   // the bot's own challenge was cast (0: none waiting)
    uint64 nextDuelScanMs = 0;    // the partner scan runs every 20-40 s
    uint32 pendingEmote = 0;      // TextEmotes id for "coa duel emote" (0: none)
    ObjectGuid emoteFace;         // who to face while emoting (the opponent, or a ring duelist)
    uint64 seenRingDuelEndMs = 0; // the ring duel end the bot already reacted to

    // Activity life.
    uint32 celebratedActivity = 0;  // the activity id the bot already celebrated (Resolving)
    uint64 nextRoamMs = 0;          // "coa activity roam": at most every 20 s
    uint64 nextIdleMs = 0;          // "coa ring idle": every 10-20 s
};

// "coa activity state".
class CoaActivityStateValue : public ManualSetValue<ActivityBotState&>
{
public:
    CoaActivityStateValue(PlayerbotAI* botAI) : ManualSetValue<ActivityBotState&>(botAI, data, "coa activity state") {}

    ActivityBotState data;
};

// The bot's map thread.
namespace CoaActivityBot
{
// The bot's "coa activity state", with `row` refreshed when the registry's version changed: the row of this bot's
// guid whose epoch equals botAI->GetCoaEpoch(), else null. Cheap when nothing changed (one acquire load).
ActivityBotState& Refresh(PlayerbotAI* botAI);

// Helpers shared by the triggers and the actions (the bot's own Player and players on its map only).
// 2D distance from the bot to the row's center; FLT_MAX when the center is unset or on another map.
float DistanceToCenter(Player* bot, ActivityMemberView const& row);
// A group member that follows the leader (not the leader, not a solo participant).
bool IsFollowing(ActivityMemberView const& row);
// The row's leader when it is on the bot's map (same map object: same instance), else null.
Player* LeaderOnMap(Player* bot, ActivityMemberView const& row);
// The player's position as an ActivityPoint (zone not looked up).
ActivityPoint PointOf(Player* player);
// A person at a client (self-bots included): the director's notion of a real player.
bool HasRealSession(Player* player);
// M5: the row's objective when it is alive, on the bot's map, attackable and within 60 yards, else null.
Unit* ObjectiveInReach(PlayerbotAI* botAI, ActivityMemberView const& row);
}  // namespace CoaActivityBot

#endif
