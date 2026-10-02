/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERSERVICE_H
#define PLAYERBOTS_PLAYERSERVICE_H

#include "ActivityKind.h"
#include <string>
#include <vector>

class PlayerbotAI;

namespace PlayerServiceCatalog
{
struct Recipe
{
    uint32 spellId = 0;
    uint32 skillId = 0;
    uint32 skillValue = 0;
    uint32 itemId = 0;
    bool hasReagents = false;  // Inventory observation only: no tool, cooldown, station or trade promise.
    std::string profession;
    std::string recipe;
    std::string item;
};

struct Offer
{
    ObjectGuid bot;
    uint64 epoch = 0;
    std::string botName;
    Recipe learned;
};

// World thread, read-only; only active learned CREATE_ITEM recipes in a profession the bot actually has.
std::vector<Recipe> Recipes(Player* bot, uint32 limit = 100);
std::vector<Offer> Offers(ActivityServices& services, Player* requester, uint32 recipeSpell = 0, uint32 limit = 20);
std::string FormatOffer(Offer const& offer);
std::vector<std::string> Lines(ActivityServices& services, Player* requester, uint32 recipeSpell = 0,
                             uint32 limit = 20);
// Exact decimal spell[:bot-counter] parsing. No names, signs, overflow, whitespace or trailing data.
bool ParseTarget(std::string const& target, uint32& spell, uint32& botLow);
bool SelfTest();
// Map thread or world-idle release, before the owned PvE guard is cleared. Cancels only this service's
// requester trade/relevant craft, and clears all native craft bookkeeping; never moves items or money.
void Release(PlayerbotAI* ai);
}

// One qualified bot, one consenting player, ordinary group following and native player-led craft/trade commands.
// This kind never configures a craft, casts a recipe, transfers inventory, or modifies anyone's money.
class PlayerServiceKind : public ActivityKind
{
public:
    ActivityKindId Id() const override { return ActivityKindId::PlayerService; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override;
    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override;
    void Tick(ActivityServices& services, Activity& activity) override;
    void OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Join(ActivityServices& services, Activity& activity, Player* player) override;
    bool Accepts(ActivityServices& services, Activity const& activity, Player* player) override;
    bool OnPlayerJoined(ActivityServices& services, Activity& activity, ActivityMember const& member) override;
    std::string Describe(Activity const& activity, uint64 nowMs) const override;
};

std::unique_ptr<ActivityKind> CreatePlayerServiceKind();

#endif
