/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_LEADERENCOUNTER_H
#define PLAYERBOTS_LEADERENCOUNTER_H

#include "ActivityConfig.h"
#include "ActivityKind.h"
#include "LeaderScaler.h"
#include <memory>
#include <vector>

struct LeaderEncounterState : ActivityKindState
{
    ActivityCampaignTarget target;
    ActivityPoint spawn;
    ActivityPoint rally;
    ActivityPoint withdrawal;
    ObjectGuid npc;
    uint32 npcEntry = 0;
    uint32 required = 0;
    uint64 formUntilMs = 0;
    uint64 gatherUntilMs = 0;
    uint64 fightUntilMs = 0;
    uint64 withdrawUntilMs = 0;
    ActivityOutcome withdrawOutcome = ActivityOutcome::Guarded;
    std::string withdrawSummary;
    LeaderScaleParameters parameters;
    std::vector<ObjectGuid> toAdd;
    bool leased = false;
    bool acceptanceQualified = false;
};

// GM-only, explicit static leader + guard whitelist. Forms a bot raid, walks to a ground-checked rally, leases
// reversible stats only when everybody is ready, then publishes an actual combat objective. No NPC respawn,
// template/database change, automated human enrollment or ordinary ambient scheduling.
class LeaderEncounterKind final : public ActivityKind
{
public:
    explicit LeaderEncounterKind(ActivityKindId mode = ActivityKindId::LeaderEncounter) : mode(mode) { }
    ActivityKindId Id() const override { return mode; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    std::optional<std::vector<std::string>> Targets(ActivityServices& services,
                                                   ActivityStartRequest const& request) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    bool Accepts(ActivityServices&, Activity const&, Player*) override { return false; }
    void OnEnd(ActivityServices& services, Activity& activity) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;

private:
    ActivityKindId mode;
};

std::unique_ptr<ActivityKind> CreateLeaderEncounterKind();
std::unique_ptr<ActivityKind> CreateCapitalRaidKind();
std::unique_ptr<ActivityKind> CreateLieutenantRaidKind();

#endif
