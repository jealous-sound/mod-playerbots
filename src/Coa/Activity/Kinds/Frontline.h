/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_FRONTLINE_H
#define PLAYERBOTS_FRONTLINE_H
#include "CampaignSupport.h"

class FrontlineKind : public CampaignKind
{
public:
    explicit FrontlineKind(bool defenders = false) : defense(defenders) {}
    ActivityKindId Id() const override { return defense ? ActivityKindId::HubDefense : ActivityKindId::Frontline; }
protected:
    CampaignSettings Settings(ActivityConfig const& config) const override;
    std::vector<ActivityCampaignTarget> const& Configured(ActivityConfig const& config) const override;
    bool Validate(ActivityServices& services, ActivityStartRequest const& request,
                  ActivityCampaignTarget const& target, CampaignState& state, std::string& why) const override;
    void Encounter(ActivityServices& services, Activity& activity, CampaignState& state) override;
private:
    bool defense;
};

std::unique_ptr<ActivityKind> CreateFrontlineKind();
std::unique_ptr<ActivityKind> CreateHubDefenseKind();
#endif
