/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaActivityStrategy.h"
#include "ActivityConfig.h"
#include "ActivityDirector.h"
#include "ActivityRegistry.h"
#include "AiObjectContext.h"
#include "CoaActivityActions.h"
#include "CoaActivityValues.h"
#include "Engine.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include <memory>
#include <optional>

void CoaActivityStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("coa activity celebrate", {NextAction("coa activity celebrate", 16.0f)}));
    triggers.push_back(new TriggerNode("coa activity travel", {NextAction("coa activity travel", 15.0f)}));
    triggers.push_back(new TriggerNode("coa activity hold", {NextAction("coa activity hold", 14.0f)}));
    triggers.push_back(new TriggerNode("coa activity away", {NextAction("coa activity return", 13.0f)}));
    triggers.push_back(new TriggerNode("coa activity catch up", {NextAction("coa activity catch up", 12.0f)}));
    triggers.push_back(new TriggerNode("coa activity roam", {NextAction("coa activity roam", 3.0f)}));
    // M5: the first blow at the kind's objective (hunts, raids); never set by the M4 kinds.
    triggers.push_back(new TriggerNode("coa activity engage", {NextAction("coa activity attack objective", 20.0f)}));
    // M5: out of combat too, RETREAT walks the bot back to the rally and off the old objective (in Resolving nothing
    // else moves it); never set by the M4 kinds.
    triggers.push_back(new TriggerNode("coa activity retreat", {NextAction("coa activity fall back", 17.0f)}));
}

void CoaActivityCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode("coa activity objective", {NextAction("coa activity attack objective", 45.0f)}));
    // M5: back to the rally, fighting or not (ACTIVITY_VIEW_RETREAT; never set by the M4 kinds).
    triggers.push_back(new TriggerNode("coa activity retreat", {NextAction("coa activity fall back", 60.0f)}));
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
constexpr uint32 GURUBASHI_ARENA_MAP = 0;
constexpr float GURUBASHI_CHEST_X = -13202.9f;  // the chest's spawn point (BootyRun.cpp's arena)
constexpr float GURUBASHI_CHEST_Y = 276.757f;
constexpr float CHEST_GUARD_YARDS = 40.0f;  // LootDistance (15) and a margin; a booty run ends with its bots beyond it
constexpr float CHEST_LOOK_YARDS = 60.0f;   // the chest is looked for this far (the pit lies below the arena's ring)

// The bot's row when it belongs to this life of the bot (a relog makes a new epoch: no participant).
std::optional<ActivityMemberView> RowOf(Player* player, PlayerbotAI* facade)
{
    std::optional<ActivityMemberView> row = ActivityRegistry::Find(player->GetGUID().GetCounter());
    if (row && row->epoch != facade->GetCoaEpoch())
        row.reset();
    return row;
}

// Gurubashi.BotsMayWin = 0: the chest is the players'. A booty-run participant never loots nor gathers; neither does
// a former one (the last row its AI saw was a booty run's: the run ended, or dropped it) while it stands near the
// chest and the chest is up. It gets both back at its next strategy reset away from the chest or once the chest is
// gone.
bool KeptFromChest(Player* player, PlayerbotAI* facade, std::optional<ActivityMemberView> const& row)
{
    std::shared_ptr<ActivityConfig const> const config = ActivityConfig::Current();
    if (!config || config->gurubashiBotsMayWin)
        return false;
    if (row && row->kind == ActivityKindId::Gurubashi)
        return true;
    if (!config->gurubashiEnable || !player->IsInWorld() || player->GetMapId() != GURUBASHI_ARENA_MAP ||
        player->GetExactDist2d(GURUBASHI_CHEST_X, GURUBASHI_CHEST_Y) > CHEST_GUARD_YARDS)
        return false;
    // Not CoaActivityBot::Refresh: at the reset that follows the leave, `row` is still the last one the AI saw.
    ActivityBotState const& s = facade->GetAiObjectContext()->GetValue<ActivityBotState&>("coa activity state")->Get();
    if (!s.row || s.row->kind != ActivityKindId::Gurubashi)
        return false;
    return player->FindNearestGameObject(ActivityThreshold::GURUBASHI_CHEST, CHEST_LOOK_YARDS, true) != nullptr;
}
}  // namespace

void CoaActivityStrategies::AddNonCombat(Player* player, PlayerbotAI* facade, Engine* engine)
{
    if (!player || !facade || !engine)
        return;

    std::optional<ActivityMemberView> const row = RowOf(player, facade);
    // M5: the objective the bot was attacking goes once the activity no longer holds it (left, another, RETREAT).
    CoaActivityAttackObjectiveAction::DropStale(facade, row ? &*row : nullptr);
    bool const keptFromChest = KeptFromChest(player, facade, row);
    if (!row)
    {
        if (keptFromChest)
        {
            engine->removeStrategy("loot", false);
            engine->removeStrategy("gather", false);
        }
        // Duel.Ambient: every free random bot may challenge whoever passes by.
        if (ActivityDirector::DuelAmbient() && !player->GetGroup())
            engine->addStrategy("coa duel", false);
        return;
    }

    engine->addStrategy("coa activity", false);
    engine->removeStrategy("lfg", false);
    engine->removeStrategy("bg", false);

    if (uint8(row->kind) >= uint8(ActivityKindId::Escort))
    {
        engine->removeStrategy("grind", false);
        engine->removeStrategy("new rpg", false);
        engine->removeStrategy("rpg", false);
        engine->removeStrategy("move random", false);
        engine->removeStrategy("travel", false);
        engine->removeStrategy("gather", false);
        if (row->kind == ActivityKindId::RareScout || row->kind == ActivityKindId::RareCamp ||
            row->kind == ActivityKindId::FestivalVisit || row->kind == ActivityKindId::PugRaid ||
            row->kind == ActivityKindId::GoldRoll || row->kind == ActivityKindId::PlayerService)
            engine->removeStrategy("loot", false);
    }

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

    // M5 solo kinds: emotes, greetings and mounts for their participants only.
    if (row->kind == ActivityKindId::Party || row->kind == ActivityKindId::Loiter ||
        row->kind == ActivityKindId::Gurubashi)
    {
        engine->removeStrategy("grind", false);
        engine->addStrategy("coa party", false);
    }

    // The Gurubashi chest is the players': with BotsMayWin = 0 nothing makes a participant loot or open it.
    if (keptFromChest)
    {
        engine->removeStrategy("loot", false);
        engine->removeStrategy("gather", false);
    }
}

void CoaActivityStrategies::AddCombat(Player* player, PlayerbotAI* facade, Engine* engine)
{
    if (!player || !facade || !engine)
        return;

    if (RowOf(player, facade))
        engine->addStrategy("coa activity combat", false);
}
