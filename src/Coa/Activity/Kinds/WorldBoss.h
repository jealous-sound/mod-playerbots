/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_COA_WORLDBOSS_H
#define PLAYERBOTS_COA_WORLDBOSS_H
#include "CampaignSupport.h"

class PlayerbotAI;
class SpellInfo;
class Unit;

class WorldBossKind : public CampaignKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::WorldBoss; }
protected:
    CampaignSettings Settings(ActivityConfig const& config) const override;
    std::vector<ActivityCampaignTarget> const& Configured(ActivityConfig const& config) const override;
    bool Validate(ActivityServices& services, ActivityStartRequest const& request,
                  ActivityCampaignTarget const& target, CampaignState& state, std::string& why) const override;
    bool Prepare(ActivityServices& services, Activity& activity, CampaignState& state) override;
    void Encounter(ActivityServices& services, Activity& activity, CampaignState& state) override;
};

namespace WorldBossTactics
{
// The owner's map thread only. Reads the immutable, epoch-checked membership snapshot.
void Update(PlayerbotAI* ai, uint64 nowMs);
bool AttackAllowed(PlayerbotAI* ai, Unit* target);
bool MovementAllowed(PlayerbotAI* ai);
Unit* ObjectiveTarget(PlayerbotAI* ai, Unit* proposed);
// Native spell preparation, including pet autocasts. Positive spells retain ordinary native handling.
bool CastAllowed(Unit* caster, SpellInfo const* spell, Unit* target);
}

std::unique_ptr<ActivityKind> CreateWorldBossKind();
#endif
