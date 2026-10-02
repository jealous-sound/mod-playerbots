/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MEMORYVIEW_H
#define PLAYERBOTS_MEMORYVIEW_H

#include "Define.h"
#include "MemoryTypes.h"
#include <array>
#include <vector>

// One relation as the bot's own strategies see it: numbers only (no name, no diary).
struct MemoryRelationView
{
    uint32 otherGuid = 0;
    uint32 otherAccount = 0;  // 0 for bots
    bool otherIsBot = false;
    uint8 fledStreak = 0;     // duels in a row they fled
    uint32 lastFledAt = 0;    // when they fled the newest of them
    std::array<int16, MEMORY_AXIS_COUNT> axes{};  // as of decayedAt: read them through MemoryView::Axis
    std::array<uint16, MEMORY_COUNTER_COUNT> counters{};
    uint32 firstSeen = 0;
    uint32 lastSeen = 0;
    uint32 decayedAt = 0;
    uint32 lastKilledMeAt = 0;  // newest kill of the bot by them outside battlegrounds, 0 = never
    uint32 bgGrudgeUntil = 0;   // a battleground kill makes a grudge until then
};

// An immutable snapshot of one bot's relations. Built by CoaMemoryMgr on the world thread, published into the
// bot's PlayerbotAI (SetMemoryView) inside CoaWorldScript::OnUpdate, and read without a lock by whoever holds the
// shared_ptr: the bot's map thread (strategies, AcceptDuelAction, EnemyPlayerValue) or the world thread. It never
// changes after construction; decay is applied on read from the half-lives copied in at build time.
class MemoryView
{
public:
    // World thread (CoaMemoryMgr). `relations` in any order: sorted here by otherGuid.
    MemoryView(uint32 botGuid, uint64 epoch, uint32 builtAt, MemoryDecay const& decay,
               std::vector<MemoryRelationView>&& relations);

    uint32 BotGuid() const { return botGuid; }
    uint64 Epoch() const { return epoch; }
    uint32 BuiltAt() const { return builtAt; }  // unix time

    // Binary search; null when the bot keeps nothing about that character.
    MemoryRelationView const* Find(uint32 otherGuid) const;
    std::vector<MemoryRelationView> const& Relations() const { return relations; }

    // One axis decayed to `now` (unix time).
    int16 Axis(MemoryRelationView const& relation, MemoryAxis axis, uint32 now) const;
    // Every axis decayed to `now`.
    std::array<int16, MEMORY_AXIS_COUNT> Axes(MemoryRelationView const& relation, uint32 now) const;
    // MemoryLabelOf the decayed axes.
    MemoryLabel Label(MemoryRelationView const& relation, uint32 now) const;

private:
    uint32 botGuid;
    uint64 epoch;
    uint32 builtAt;
    MemoryDecay decay;
    std::vector<MemoryRelationView> relations;  // sorted by otherGuid
};

#endif
