/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RARESCOUT_H
#define PLAYERBOTS_RARESCOUT_H

#include "ActivityGuards.h"
#include "ActivityKind.h"
#include <memory>
#include <optional>
#include <unordered_map>

enum class RareScoutObservation : uint8
{
    Unloaded = 0,
    Dead,
    Alive
};

enum class RareScoutProgress : uint8
{
    Waiting = 0,
    Success,
    Timeout
};

// Pure rules also usable by the console harness; no map or creature is needed.
namespace RareScoutRules
{
constexpr uint64 DWELL_MS = 30000;
constexpr uint64 RETIRE_MS = 60000;

constexpr bool AcceptsTemplate(uint32 rank, uint32 minLevel, uint32 maxLevel, bool questGiver, bool openWorld)
{
    return (rank == 2 || rank == 4) && minLevel >= 1 && minLevel <= maxLevel && maxLevel <= 60 &&
           !questGiver && openWorld;
}

constexpr RareScoutProgress ProgressAt(uint64 nowMs, uint64 deadlineMs, std::optional<uint64> sightedAtMs,
                                      bool announced)
{
    if (nowMs >= deadlineMs)
        return RareScoutProgress::Timeout;
    if (announced && sightedAtMs && nowMs >= *sightedAtMs && nowMs - *sightedAtMs >= DWELL_MS)
        return RareScoutProgress::Success;
    return RareScoutProgress::Waiting;
}

constexpr RareScoutProgress RetirementAt(uint64 nowMs, uint64 deadlineMs, bool arrived, bool inCombat)
{
    if (nowMs >= deadlineMs)
        return RareScoutProgress::Timeout;
    return arrived && !inCombat ? RareScoutProgress::Success : RareScoutProgress::Waiting;
}

// Exercise template exclusions and the delivery/deadline boundaries without world objects. In particular,
// an observed rare cannot complete successfully while its channel announcement is still budget-deferred.
inline bool SelfTest()
{
    for (uint32 rank = 0; rank <= 5; ++rank)
        if (AcceptsTemplate(rank, 1, 60, false, true) != (rank == 2 || rank == 4))
            return false;
    if (AcceptsTemplate(4, 0, 60, false, true) || AcceptsTemplate(2, 1, 61, false, true) ||
        AcceptsTemplate(4, 30, 29, false, true) || AcceptsTemplate(2, 1, 60, true, true) ||
        AcceptsTemplate(4, 1, 60, false, false))
        return false;
    return ProgressAt(60000, 100000, std::nullopt, false) == RareScoutProgress::Waiting &&
           ProgressAt(60000, 100000, std::nullopt, true) == RareScoutProgress::Waiting &&
           ProgressAt(60000, 100000, 0, false) == RareScoutProgress::Waiting &&
           ProgressAt(29999, 100000, 0, true) == RareScoutProgress::Waiting &&
           ProgressAt(30000, 100000, 0, true) == RareScoutProgress::Success &&
           ProgressAt(60000, 100000, 1000, true) == RareScoutProgress::Success &&
           ProgressAt(999, 100000, 1000, true) == RareScoutProgress::Waiting &&
           ProgressAt(100000, 100000, 0, true) == RareScoutProgress::Timeout &&
           ProgressAt(100001, 100000, 0, false) == RareScoutProgress::Timeout &&
           ProgressAt(100000, 100000, std::nullopt, false) == RareScoutProgress::Timeout &&
           RetirementAt(999, 1000, false, false) == RareScoutProgress::Waiting &&
           RetirementAt(999, 1000, true, true) == RareScoutProgress::Waiting &&
           RetirementAt(999, 1000, true, false) == RareScoutProgress::Success &&
           RetirementAt(1000, 1000, true, false) == RareScoutProgress::Timeout &&
           RetirementAt(1001, 1000, false, true) == RareScoutProgress::Timeout;
}
}  // namespace RareScoutRules

struct RareScoutState : ActivityKindState
{
    ActivitySpawnInfo spawn;
    ActivityPoint camp;
    ActivityPoint escape;
    ObjectGuid scout;
    uint64 scoutEpoch = 0;
    ObjectGuid seenCreature;
    RareScoutObservation observation = RareScoutObservation::Unloaded;
    std::optional<uint64> sightedAtMs;
    uint64 nextPostMs = 0;
    uint64 waitUntilMs = 0;
    uint64 retireUntilMs = 0;
    float escapeMinDistance = 0.0f;
    ActivityOutcome retireOutcome = ActivityOutcome::None;
    std::string retireSummary;
    bool announced = false;
};

// One nearby bot walks to a camp, reports a visible live rare at most once, then walks out of its sight range.
// No group, objective, teleport, creature mutation or reward. The director publishes the solo activity's view;
// the map-thread RareScout safety seam suppresses proactive attacks, grinding, gathering and loot.
class RareScoutKind final : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::RareScout; }
    std::optional<std::vector<std::string>> Targets(ActivityServices& services,
                                                   ActivityStartRequest const& request) override;
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    bool Accepts(ActivityServices&, Activity const&, Player*) override { return false; }
    void OnEnd(ActivityServices& services, Activity& activity) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;

private:
    // Spawn ids, not target aliases: two names of the same spawn cannot start two observers.
    std::unordered_map<uint32, uint32> spawnInUse;
};

std::unique_ptr<ActivityKind> CreateRareScoutKind();

#endif
