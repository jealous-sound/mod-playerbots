/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYREGISTRY_H
#define PLAYERBOTS_ACTIVITYREGISTRY_H

#include "ActivityTypes.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

// One participant as the bots see it: a copy of what the director knows about that bot's part in its activity.
struct ActivityMemberView
{
    uint32 activityId = 0;
    uint64 epoch = 0;                  // the bot's CoaEpoch at enrollment: another life of the bot is no participant
    ActivityKindId kind = ActivityKindId::None;
    ActivityRole role = ActivityRole::Member;
    ActivityPhase phase = ActivityPhase::Planning;
    uint64 phaseSinceMs = 0;           // CoaTime::NowMs() when the phase began
    uint32 leaderLow = 0;              // group kinds: the leader's guid counter; 0 for solo kinds
    uint32 groupLow = 0;               // the activity's group, 0 before it is formed and for solo kinds
    ActivityPoint center;              // Gathering: the rally; Marching: the next stop; Active: the ring or hunt center
    float radius = 0.0f;               // Gathering/Marching: arrival radius; Active: leash radius
    ObjectGuid objective;              // M5: the target held in combat ("coa activity combat"); empty in M4
    uint32 pvpRuleset = 0;
    uint64 ringDuelEndMs = 0;          // duel rings: when the last duel between ring members ended (spectators cheer)
    uint8 flags = 0;                   // ActivityViewFlag bits
    uint8 style = 0;                   // M5: ActivityViewStyle of solo kinds ("coa party"); 0 for every other kind
    uint32 emote = 0;                  // M5: TextEmotes id every bot plays when it celebrates (0: its own pick)

    bool Has(ActivityViewFlag flag) const { return (flags & flag) != 0; }
};

// What map threads read: every participant, every reserved bot, every activity group. Built by the director on the
// world thread and never changed after Publish; readers keep their shared_ptr as long as they like.
struct ActivityRegistrySnapshot
{
    uint64 version = 0;                                      // Publish's counter
    std::unordered_map<uint32, ActivityMemberView> members;  // bot guid counter -> its row
    std::unordered_set<uint32> reserved;                     // bots being asked (Planning): not free for anything else
    std::unordered_map<uint32, uint32> groups;               // group guid counter -> activity id

    ActivityMemberView const* Find(uint32 botLow) const
    {
        auto const it = members.find(botLow);
        return it == members.end() ? nullptr : &it->second;
    }
    bool IsReserved(uint32 botLow) const { return reserved.count(botLow) != 0; }
    bool IsHeld(uint32 botLow) const { return members.count(botLow) != 0 || reserved.count(botLow) != 0; }
};

// The published snapshot. The director is its only writer (world thread, inside CoaWorldScript::OnUpdate, after the
// map threads were joined); every other reader, on any thread, copies the shared_ptr under a tiny mutex. AnyHeld()
// lets every legacy seam return after one relaxed load when nothing runs (always, with CoaActivity.Enable = 0).
class ActivityRegistry
{
public:
    static ActivityRegistry& Instance();

    // World thread, the director only. Stamps snapshot->version (the previous version + 1) before storing it, then
    // sets AnyHeld() (members or reserved not empty) and Version(). Null publishes an empty snapshot.
    void Publish(std::shared_ptr<ActivityRegistrySnapshot> snapshot);

    // Any thread. Never null (an empty snapshot before the first Publish).
    std::shared_ptr<ActivityRegistrySnapshot const> Current() const;

    // Any thread, one relaxed load: something is held (a participant or a reserved bot).
    static bool AnyHeld() { return anyHeld.load(std::memory_order_relaxed); }
    // Any thread: the version of the last Publish (acquire). Bots re-read their row only when it changed.
    static uint64 Version() { return version.load(std::memory_order_acquire); }

    // The seams' questions. Any thread; each returns at once (false/nullopt) when !AnyHeld().
    // A participant of a live activity (any phase but Planning).
    static bool IsParticipant(uint32 botLow);
    // A participant or a bot reserved by a plan.
    static bool IsHeld(uint32 botLow);
    // A group an activity formed.
    static bool IsActivityGroup(ObjectGuid groupGuid);
    // The bot's row (a copy).
    static std::optional<ActivityMemberView> Find(uint32 botLow);

private:
    ActivityRegistry();
    ActivityRegistry(ActivityRegistry const&) = delete;
    ActivityRegistry& operator=(ActivityRegistry const&) = delete;

    mutable std::mutex lock;                                  // guards current only
    std::shared_ptr<ActivityRegistrySnapshot const> current;  // never null
    inline static std::atomic<bool> anyHeld{false};
    inline static std::atomic<uint64> version{0};
};

#endif
