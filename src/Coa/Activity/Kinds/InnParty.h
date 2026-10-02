/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INNPARTY_H
#define PLAYERBOTS_INNPARTY_H

#include "ActivityKind.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ActivitySpot;

// An inn party's own state (ActivityPlan::state, then Activity::state).
struct InnPartyState : ActivityKindState
{
    std::string spotName;        // a Party.Spots name, or "inn_<spawn>" for an innkeeper
    ActivityPoint spot;          // the innkeeper's position (or the spot), z on the ground, zone set
    uint32 areaId = 0;           // the area at the spot (%area, the activity's name)
    uint8 style = ACTIVITY_STYLE_INN;  // ACTIVITY_STYLE_INN or ACTIVITY_STYLE_DANCE (Party.DancePercent)
    uint64 activeUntilMs = 0;    // Active ends here (Party.Minutes +- 5)
    bool called = false;         // coa_act_party_call posted (or skipped)
    // Real player guid counter -> CoaTime::NowMs() of the greeting line said to it (once per player per party).
    std::unordered_map<uint32, uint64> greetedMs;
    uint32 greetings = 0;        // greeting lines said (at most PARTY_GREETINGS_MAX)
};

// "party" (M5): 4-8 bots within +-Party.LevelSpread of a real player's level gather at the friendly inn nearest that
// player (an innkeeper within Party.MaxYards, or a Party.Spots spot) for Party.Minutes +- 5: they sit, drink, toast,
// laugh, dance (a dance party with Party.DancePercent), greet real players who come in (the "coa party" strategy:
// emotes only for participants; one coa_act_party_call post in the zone channels, one coa_act_party_greet line per
// player, said by the director's operations). Solo participants, no group. Contract section 9.1.
class InnPartyKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::Party; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    void OnEnd(ActivityServices& services, Activity& activity) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;

private:
    // Every innkeeper spawn of the overworld maps (npcflag UNIT_NPC_FLAG_INNKEEPER), read once from
    // ObjectMgr::GetAllCreatureData on the first Plan (a cache: the world data never changes while the server runs).
    struct Inn
    {
        uint32 spawnId = 0;
        uint32 factionTemplate = 0;  // creature_template.faction: friendly to the party's team, or neutral
        ActivityPoint point;         // zone looked up when chosen
    };
    std::vector<Inn> inns;
    bool innsLoaded = false;
    // A Party.Spots spot's check (ground height), cached by its whole definition as DuelGatheringKind does; one
    // LOG_WARN per bad definition.
    struct SpotCheck
    {
        bool good = false;
        ActivityPoint point;
        uint32 areaId = 0;
    };
    SpotCheck const& Check(ActivitySpot const& spot);
    void LoadInns();
    std::unordered_map<std::string, SpotCheck> spotChecks;
    // Spot or inn name -> the live party there (one party per inn).
    std::unordered_map<std::string, uint32> spotInUse;
};

std::unique_ptr<ActivityKind> CreateInnPartyKind();

#endif
