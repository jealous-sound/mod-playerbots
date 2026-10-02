/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GOLDROLL_H
#define PLAYERBOTS_GOLDROLL_H

#include "ActivityKind.h"

class PlayerbotAI;

namespace GoldRollRules
{
// Two fair 1..100 dice are frozen once after consent. A tie has no payment and is never rerolled.
inline int Winner(uint32 bot, uint32 player)
{
    return bot > player ? 1 : bot < player ? -1 : 0;
}
inline bool ExactOffer(uint32 stake, bool botWins, uint32 botMoney, uint32 playerMoney,
                       bool items, bool spell)
{
    return stake && !items && !spell && botMoney == (botWins ? 0u : stake) &&
           playerMoney == (botWins ? stake : 0u);
}
inline bool Settled(uint32 stake, bool botWins, uint32 beforeBot, uint32 beforePlayer,
                     uint32 afterBot, uint32 afterPlayer)
{
    if (!stake)
        return false;
    return botWins ? uint64(afterBot) == uint64(beforeBot) + stake && beforePlayer >= stake &&
                     afterPlayer == beforePlayer - stake :
                     uint64(afterPlayer) == uint64(beforePlayer) + stake && beforeBot >= stake &&
                     afterBot == beforeBot - stake;
}
bool ParseTarget(std::string const& target, uint32& botLow, uint32& stake);
bool SelfTest();
}

namespace GoldRollActions
{
// Map thread. Root routes active GoldRoll trade-status packets here instead of generic priced item trading.
bool Active(PlayerbotAI* ai);
void Tick(PlayerbotAI* ai);
void Release(PlayerbotAI* ai);
}

// Explicit proposal + command acceptance + ordinary human native trade acceptance. No automatic debit or escrow.
class GoldRollKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::GoldRoll; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    bool Accept(ActivityServices& services, Activity& activity, Player* player, std::string& why);
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    bool Accepts(ActivityServices&, Activity const&, Player*) override { return false; }
    void OnEnd(ActivityServices& services, Activity& activity) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;
};

std::unique_ptr<ActivityKind> CreateGoldRollKind();

#endif
