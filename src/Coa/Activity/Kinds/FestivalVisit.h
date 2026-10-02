/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FESTIVALVISIT_H
#define PLAYERBOTS_FESTIVALVISIT_H

#include "ActivityKind.h"

class PlayerbotAI;
class SpellInfo;
class Unit;

namespace FestivalVisitActions
{
// Bot map thread: only this bot's native gossip/item use/channel; checks its current registry id and epoch.
// Call after the PvE guard each update, including the first update after activity release (cleanup only).
void Tick(PlayerbotAI* ai);
void Release(PlayerbotAI* ai);
bool AllowsCast(PlayerbotAI* ai, SpellInfo const* spell, Unit* target);
// Pure whitelist boundaries; production uses this same predicate before touching a native interaction.
bool Supported(std::string const& interaction, uint32 event, uint32 entry);
bool SelfTest();
}

// One visitor at an explicitly configured event spawn. No groups, objectives, teleports or quest turn-ins.
class FestivalVisitKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::FestivalVisit; }
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
};

std::unique_ptr<ActivityKind> CreateFestivalVisitKind();

#endif
