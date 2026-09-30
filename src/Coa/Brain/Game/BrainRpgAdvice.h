/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINRPGADVICE_H
#define PLAYERBOTS_BRAINRPGADVICE_H

#include "Define.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

// M6 RPG blend (CoaBrain.Use.RpgNext). Random bots in the NewRpg strategy pick their next status by weighted dice
// (NewRpgBaseAction::RandomChangeStatus). This asks the brain, once per (zone, level band, role) group that has
// random bots in NewRpg and for one advice TTL, which status comes next; the answer becomes a probability per
// status, published as an immutable snapshot that the status pick reads on the map threads:
//
//     w' = w x (1 - a + a x k x p)      a = CoaBrain.Rpg.Influence, k = statuses in the pick, p = advice probability
//
// With RpgNext = 0 (or no advice for a bot's group) the pick is exactly today's: same weights, same draw.

// The eight statuses the brain chooses among, in option order. Idle has no option: its weight is never changed.
enum class BrainRpgOption : uint8
{
    WanderRandom = 0,
    WanderNpc,
    GoGrind,
    GoCamp,
    DoQuest,
    TravelFlight,
    Rest,
    OutdoorPvp,
    Count
};

constexpr std::size_t BRAIN_RPG_OPTIONS = std::size_t(BrainRpgOption::Count);

// "wander_random", "wander_npc", "go_grind", "go_camp", "do_quest", "travel_flight", "rest", "outdoor_pvp": the
// option keys of the brain question, in option order.
char const* BrainRpgOptionKey(BrainRpgOption option);

// The group advice is shared by: the zone id, the level band (level / 10: 0..6), the role (0 dps, 1 tank, 2 healer).
struct BrainRpgKey
{
    uint32 zoneId = 0;
    uint8 band = 0;
    uint8 role = 0;

    uint64 Packed() const { return (uint64(zoneId) << 16) | (uint64(band) << 8) | role; }
};

// One group's advice. p[i] is the brain's probability of option i (they sum to about 1), or -1 when missing.
struct BrainRpgEntry
{
    std::array<float, BRAIN_RPG_OPTIONS> p{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    uint64 untilMs = 0;  // CoaTime::NowMs() clock: the advice expires here
    uint32 bots = 0;     // random bots of the group when it was asked
};

// What the map threads read: built by the world thread, never changed after it is published.
struct BrainRpgSnapshot
{
    uint64 version = 0;
    std::unordered_map<uint64, BrainRpgEntry> entries;  // BrainRpgKey::Packed() -> advice

    BrainRpgEntry const* Find(BrainRpgKey const& key) const
    {
        auto const it = entries.find(key.Packed());
        return it == entries.end() ? nullptr : &it->second;
    }
};

class BrainRpgAdvice
{
public:
    static BrainRpgAdvice& Instance();

    // Any thread, one relaxed load: CoaBrain.Use.RpgNext is on, the brain runs, and the snapshot has an entry. The
    // seam returns at once when this is false, so with RpgNext = 0 it costs one load per status pick.
    static bool Active() { return active.load(std::memory_order_relaxed); }

    // Any thread. The published snapshot (never null; empty before the first entry). A shared_ptr copy under a tiny
    // mutex: map threads call it once per status pick, which is rare (seconds per bot).
    static std::shared_ptr<BrainRpgSnapshot const> Current();

    // Pure. The option of a NewRpgStatus value (PlayerbotAIConfig.h): WANDER_RANDOM 3 -> WanderRandom, WANDER_NPC 4,
    // GO_GRIND 1, GO_CAMP 2, DO_QUEST 5, TRAVEL_FLIGHT 6, REST 7, OUTDOOR_PVP 8; -1 for RPG_IDLE, RPG_STATUS_END and
    // anything else (Blend leaves such a status' weight as it is).
    static int32 OptionOfStatus(int32 newRpgStatus);

    // Pure. The group of a bot: its zone, level / 10 (at most 6), role (PlayerbotAI::IsTank -> 1, IsHeal -> 2, else
    // 0). Map thread or world thread, for the bot's own thread only.
    static BrainRpgKey KeyOf(Player* bot);

    // Pure. The formula on `count` candidate statuses: `options[i]` is status i's option (-1: none) and weights[i] its
    // configured integer weight. The statuses with weight > 0, an option and a known p are the k "eligible" ones; their
    // p are renormalized to sum to 1 over them; w' = max(1, round(16 x w x (1 - influence + influence x k x p)));
    // every other status gets 16 x w. Returns false, leaving `weights` untouched, when fewer than 2 statuses are
    // eligible, the p sum is not positive, or influence is not positive. The factor 16 keeps the integers' resolution
    // (the seam draws urand(1, sum) on the scaled weights). Never turns an eligible weight into 0.
    static bool BlendWeights(BrainRpgEntry const& entry, float influence, int32 const* options, uint32* weights,
                             std::size_t count);

    // Map thread, the seam in NewRpgBaseAction::RandomChangeStatus. Looks the bot's group up in the snapshot (an
    // entry past its untilMs is ignored) and runs BlendWeights with CoaBrain.Rpg.Influence; counts
    // BrainStats::rpgApplied when it changed the weights. False (weights untouched) whenever Active() is false, there
    // is no fresh entry, or BlendWeights says so. `statuses` holds NewRpgStatus values.
    static bool Blend(Player* bot, int32 const* statuses, uint32* weights, std::size_t count);

    // World thread, CoaWorldScript::OnUpdate (every tick, once the brain ever ran). Every Rpg tick (10 s): drains
    // the advice sink (answers become entries, merged into a new snapshot; expired entries leave it); when Use.RpgNext
    // is on and the brain is active, counts the random bots in NewRpg per group (BrainRpgAdvice::KeyOf; map thread
    // state is safe to read here: the map threads are idle) and submits at most 2 jobs per tick, for the largest
    // groups of at least Rpg.MinBots bots with no fresh entry and no job waiting, at most 24 jobs waiting in all.
    void Update(uint64 nowMs);

    // World thread, CoaWorldScript::OnAfterConfigLoad(reload): Use.RpgNext turned off empties the snapshot and cancels
    // the waiting jobs at once (the next pick is today's); turned on, the manager starts over.
    void OnConfigReload();

    // World thread, WORLDHOOK_ON_SHUTDOWN before the brain stops: cancels waiting jobs, closes the sink, clears.
    void Shutdown();

    // World thread (.playerbots brain rpg status|clear). Status lines: on/off, influence, TTL, groups asked about,
    // each entry (zone name, band, role, bots, age, the probabilities). Clear drops the snapshot and the jobs.
    std::vector<std::string> FormatStatus() const;
    std::vector<std::string> Clear();

private:
    BrainRpgAdvice() = default;
    BrainRpgAdvice(BrainRpgAdvice const&) = delete;
    BrainRpgAdvice& operator=(BrainRpgAdvice const&) = delete;

    struct Impl;  // the sink, the waiting jobs, the tick clock: world-thread state, defined in BrainRpgAdvice.cpp
    std::shared_ptr<Impl> impl;

    inline static std::atomic<bool> active{false};
};

#endif
