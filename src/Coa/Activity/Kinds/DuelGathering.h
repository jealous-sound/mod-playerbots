/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUELGATHERING_H
#define PLAYERBOTS_DUELGATHERING_H

#include "ActivityKind.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <memory>
#include <string>
#include <unordered_map>

struct ActivityDuelSpot;

// A duel ring's own state (Activity::state).
struct DuelGatheringState : ActivityKindState
{
    std::string spotName;
    uint32 spotAreaId = 0;       // the area at the spot (lines: %area)
    uint64 activeUntilMs = 0;    // Active ends here (Duel.Minutes +- 10)
    uint32 duelsFought = 0;      // duels between ring members
    uint32 duelsWithPlayers = 0; // duels between a ring member and a real player
    bool opened = false;         // the opening line was said (or given up)
    bool posted = false;         // the Chat.Channel announcement was made (or skipped)
    bool championDone = false;   // the champion's closing line was said (or skipped)
    bool closed = false;         // the closing lines were said
};

// "duel": 4-8 bots of one faction and a +-3 level window gather at a duel spot, duel each other and passing players
// for 20-40 minutes, cheer and cry, then scatter. Solo participants, no group. At most Duel.Max rings (1).
// Contract section 12.
class DuelGatheringKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::Duel; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnDuelResult(ActivityServices& services, Activity& activity, ActivityDuelResult const& result) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;

private:
    // A spot's check (ground height, area with AREA_FLAG_ALLOW_DUELS), cached by the spot's whole definition, so a
    // reload that moves a spot checks it again. One LOG_WARN per bad definition.
    struct SpotCheck
    {
        bool good = false;
        ActivityPoint point;  // z on the ground, zone set
        uint32 areaId = 0;
    };

    SpotCheck const& Check(ActivityDuelSpot const& spot);

    std::unordered_map<std::string, SpotCheck> spotChecks;
};

std::unique_ptr<ActivityKind> CreateDuelGatheringKind();

#endif
