/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_PUGRAID_H
#define PLAYERBOTS_PUGRAID_H
#include "CampaignSupport.h"
class PlayerbotAI;

struct PugRaidState : CampaignState
{
    uint32 reserveLimit = 1;
    std::map<ObjectGuid, std::set<uint32>> reserves;
    std::map<std::pair<ObjectGuid, uint8>, ObjectGuid> winners;
    uint64 lootUntilMs = 0;
    uint64 entryUntilMs = 0;
    bool entered = false;
    bool unclaimedLoot = false;
    uint32 portalTrigger = 0;
    ActivityPoint portal;
    std::map<ObjectGuid, uint64> entryAttemptMs;
};

namespace PugRaidTactics
{
void Update(PlayerbotAI* ai, uint64 nowMs);
}

class PugRaidKind : public CampaignKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::PugRaid; }
    std::string Reserve(ActivityServices& services, Activity& activity, Player* player, uint32 itemId);
protected:
    CampaignSettings Settings(ActivityConfig const& config) const override;
    std::vector<ActivityCampaignTarget> const& Configured(ActivityConfig const& config) const override;
    std::unique_ptr<CampaignState> NewState() const override { return std::make_unique<PugRaidState>(); }
    bool Validate(ActivityServices& services, ActivityStartRequest const& request,
                  ActivityCampaignTarget const& target, CampaignState& state, std::string& why) const override;
    bool Prepare(ActivityServices& services, Activity& activity, CampaignState& state) override;
    bool Transitioning(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    void Encounter(ActivityServices& services, Activity& activity, CampaignState& state) override;
private:
    bool Allocate(ActivityServices& services, Activity& activity, PugRaidState& state);
};

std::unique_ptr<ActivityKind> CreatePugRaidKind();
#endif
