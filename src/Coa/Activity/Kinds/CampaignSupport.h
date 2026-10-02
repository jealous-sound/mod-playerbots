/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_CAMPAIGNSUPPORT_H
#define PLAYERBOTS_CAMPAIGNSUPPORT_H

#include "ActivityConfig.h"
#include "ActivityGuards.h"
#include "ActivityKind.h"
#include <map>
#include <set>

class Creature;
class Group;

struct CampaignSettings
{
    bool enabled = false;
    uint32 minimum = 3;
    uint32 maximum = 5;
    uint32 recruitSeconds = 60;
    uint32 minutes = 15;
    uint32 cooldownMinutes = 0;
    bool raid = false;
    bool instance = false;
    bool defense = false;
    bool anchor = false;
    uint32 pvpRuleset = 0;
    uint8 lowest = 1;
    uint8 highest = 60;
};

struct CampaignState : ActivityKindState
{
    ActivityCampaignTarget target;
    ActivitySpawnInfo spawn;
    ActivityPoint rally;
    CampaignSettings settings;
    uint64 formUntilMs = 0;
    uint64 recruitUntilMs = 0;
    uint64 gatherUntilMs = 0;
    uint64 timeoutMs = 0;
    uint64 nextPostMs = 0;
    uint64 engagedSinceMs = 0;
    bool engaged = false;
    ObjectGuid engagedGuid;
    std::vector<ObjectGuid> toAdd;
    // Only strategies introduced by this activity are removed. Original engine choices remain intact.
    std::map<ObjectGuid, uint8> addedStrategy;
    std::map<ObjectGuid, uint64> strategyEpoch;
};

namespace CampaignRules
{
inline bool SupportedRaid(uint32 map, std::string const& strategy)
{
    return (map == 409 && strategy == "moltencore") || (map == 469 && strategy == "bwl") ||
           (map == 509 && strategy == "aq20");
}
inline bool SupportedBoss(uint32 entry)
{
    return entry == 6109 || entry == 14887 || entry == 14888 || entry == 14889 || entry == 14890;
}
inline bool NeedsHistory(bool gm, uint32 completed, uint32 required)
{
    return !gm && completed < required;
}
inline bool ReserveEligible(bool reservedItem, bool bot, bool hasReserve, bool member)
{
    return member && (!reservedItem || (!bot && hasReserve));
}
inline bool ReflectCastAllowed(bool reflection, bool negativeMagic, bool hitsBoss)
{
    return !reflection || !negativeMagic || !hitsBoss;
}
inline bool SelfTest()
{
    return SupportedRaid(409, "moltencore") && SupportedRaid(469, "bwl") && SupportedRaid(509, "aq20") &&
        !SupportedRaid(309, "zg") && !SupportedRaid(409, "bwl") && SupportedBoss(6109) &&
        SupportedBoss(14890) && !SupportedBoss(10184) && NeedsHistory(false, 9, 10) &&
        !NeedsHistory(false, 10, 10) && !NeedsHistory(true, 0, 10) &&
        !ReserveEligible(true, true, true, true) && !ReserveEligible(true, false, false, true) &&
        !ReserveEligible(true, false, true, false) && ReserveEligible(true, false, true, true) &&
        ReserveEligible(false, true, false, true) && !ReserveEligible(false, true, false, false) &&
        !ReflectCastAllowed(true, true, true) && ReflectCastAllowed(true, false, true) &&
        ReflectCastAllowed(true, true, false) && ReflectCastAllowed(false, true, true);
}
}

namespace CampaignWorld
{
bool Safe(Player* player, bool allowInstance = false, uint32 pvpRuleset = 0);
Group* GroupOf(Activity const& activity);
CampaignState* State(Activity& activity);
bool In(Activity const& activity, ObjectGuid guid);
bool GroupEngaged(Activity const& activity, Creature* creature);
bool OutsiderEngaged(Activity const& activity, Creature* creature);
Creature* SpawnFor(Activity const& activity, CampaignState const& state);
bool Roles(Activity const& activity, uint32 tanks, uint32 heals);
bool GuardLevels(ActivityCampaignTarget const& target, std::string& why, float yards = 150.0f,
                 bool completeCensus = true);
bool ResolveTarget(ActivityCampaignTarget const& target, ActivitySpawnInfo& spawn, std::string& why,
                   bool allowInstance = false);
void Strategy(Activity& activity, CampaignState& state);
void RestoreStrategies(Activity& activity, CampaignState& state);
}

// The common lifecycle owns registered groups and bounded recruitment. Concrete kinds own encounter behavior.
class CampaignKind : public ActivityKind
{
public:
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    bool Accepts(ActivityServices& services, Activity const& activity, Player* player) override;
    void OnEnd(ActivityServices& services, Activity& activity) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;
    std::optional<std::vector<std::string>> Targets(ActivityServices& services,
                                                   ActivityStartRequest const& request) override;

protected:
    virtual CampaignSettings Settings(ActivityConfig const& config) const = 0;
    virtual std::vector<ActivityCampaignTarget> const& Configured(ActivityConfig const& config) const = 0;
    virtual bool Validate(ActivityServices& services, ActivityStartRequest const& request,
                          ActivityCampaignTarget const& target, CampaignState& state, std::string& why) const = 0;
    virtual std::unique_ptr<CampaignState> NewState() const { return std::make_unique<CampaignState>(); }
    virtual bool Prepare(ActivityServices&, Activity&, CampaignState&) { return true; }
    virtual bool Transitioning(ActivityServices&, Activity&, ActivityMember const&) { return false; }
    virtual void Encounter(ActivityServices& services, Activity& activity, CampaignState& state);
    virtual void Cleanup(ActivityServices&, Activity&, CampaignState&) {}
    std::map<uint32, uint64> lastStarted;  // originating spawn; aliases cannot evade cooldown
};

#endif
