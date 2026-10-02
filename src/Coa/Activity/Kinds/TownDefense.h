/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_TOWNDEFENSE_H
#define PLAYERBOTS_TOWNDEFENSE_H
#include "CampaignSupport.h"

class Spell;

namespace TownDefenseWave
{
// Native CanPrepare: only an explicitly targeted owned marked wave rejects inventory-on-death auras.
bool CanPrepare(Spell* spell);
bool SelfTest();
}

struct TownDefenseState : CampaignState
{
    std::vector<ObjectGuid> wave;
    std::set<ObjectGuid> verifiedDead;
    uint32 waves = 0;
    uint32 clearedCorpseTags = 0;
    uint32 wantedWaves = 0;
    uint32 waveSize = 0;
    uint64 nextWaveMs = 0;
    uint64 waveDeadlineMs = 0;
    uint64 cleanupDeadlineMs = 0;
    bool cleaning = false;
};

class TownDefenseKind : public CampaignKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::TownDefense; }
    std::string Describe(Activity const& activity, uint64 nowMs) const override;
protected:
    CampaignSettings Settings(ActivityConfig const& config) const override;
    std::vector<ActivityCampaignTarget> const& Configured(ActivityConfig const& config) const override;
    std::unique_ptr<CampaignState> NewState() const override { return std::make_unique<TownDefenseState>(); }
    bool Validate(ActivityServices& services, ActivityStartRequest const& request,
                  ActivityCampaignTarget const& target, CampaignState& state, std::string& why) const override;
    bool Prepare(ActivityServices& services, Activity& activity, CampaignState& state) override;
    void Encounter(ActivityServices& services, Activity& activity, CampaignState& state) override;
    void Cleanup(ActivityServices& services, Activity& activity, CampaignState& state) override;
};

std::unique_ptr<ActivityKind> CreateTownDefenseKind();
#endif
