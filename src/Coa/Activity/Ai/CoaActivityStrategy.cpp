/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaActivityStrategy.h"
#include "ActivityDirector.h"
#include "ActivityRegistry.h"
#include "Engine.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include <optional>

void CoaActivityStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("coa activity celebrate", {NextAction("coa activity celebrate", 16.0f)}));
    triggers.push_back(new TriggerNode("coa activity travel", {NextAction("coa activity travel", 15.0f)}));
    triggers.push_back(new TriggerNode("coa activity hold", {NextAction("coa activity hold", 14.0f)}));
    triggers.push_back(new TriggerNode("coa activity away", {NextAction("coa activity return", 13.0f)}));
    triggers.push_back(new TriggerNode("coa activity catch up", {NextAction("coa activity catch up", 12.0f)}));
    triggers.push_back(new TriggerNode("coa activity roam", {NextAction("coa activity roam", 3.0f)}));
}

void CoaActivityCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("coa activity objective", {NextAction("coa activity attack objective", 45.0f)}));
}

void CoaDuelStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("coa duel ended", {NextAction("coa duel emote", 16.0f)}));
    triggers.push_back(new TriggerNode("coa duel unanswered", {NextAction("coa duel cancel", 9.0f)}));
    triggers.push_back(new TriggerNode("coa duel partner", {NextAction("coa challenge duel", 8.0f)}));
    triggers.push_back(new TriggerNode("coa ring idle", {NextAction("coa ring idle", 2.0f)}));
}

namespace
{
// The bot's row when it belongs to this life of the bot (a relog makes a new epoch: no participant).
std::optional<ActivityMemberView> RowOf(Player* player, PlayerbotAI* facade)
{
    std::optional<ActivityMemberView> row = ActivityRegistry::Find(player->GetGUID().GetCounter());
    if (row && row->epoch != facade->GetCoaEpoch())
        row.reset();
    return row;
}
}  // namespace

void CoaActivityStrategies::AddNonCombat(Player* player, PlayerbotAI* facade, Engine* engine)
{
    if (!player || !facade || !engine)
        return;

    std::optional<ActivityMemberView> const row = RowOf(player, facade);
    if (!row)
    {
        // Duel.Ambient: every free random bot may challenge whoever passes by.
        if (ActivityDirector::DuelAmbient() && !player->GetGroup())
            engine->addStrategy("coa duel", false);
        return;
    }

    engine->addStrategy("coa activity", false);
    engine->removeStrategy("lfg", false);
    engine->removeStrategy("bg", false);

    // Solo participants and the leader (AiFactory's own test; a gang in Forming has no group yet): the activity
    // decides where they go. A gang leader keeps "grind": it hunts with it.
    if (!player->GetGroup() || player->GetGroup()->GetLeaderGUID() == player->GetGUID())
    {
        engine->removeStrategy("new rpg", false);
        engine->removeStrategy("rpg", false);
        engine->removeStrategy("move random", false);
        engine->removeStrategy("travel", false);
    }

    if (row->kind == ActivityKindId::Duel)
    {
        engine->removeStrategy("grind", false);
        engine->addStrategy("coa duel", false);
    }
}

void CoaActivityStrategies::AddCombat(Player* player, PlayerbotAI* facade, Engine* engine)
{
    if (!player || !facade || !engine)
        return;

    if (RowOf(player, facade))
        engine->addStrategy("coa activity combat", false);
}
