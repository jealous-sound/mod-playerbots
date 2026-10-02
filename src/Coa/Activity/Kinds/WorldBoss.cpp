/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#include "WorldBoss.h"
#include "ActivityDirector.h"
#include "AiObjectContext.h"
#include "AttackAction.h"
#include "CellImpl.h"
#include "CoaActivityValues.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <cmath>
#include <list>

namespace
{
constexpr uint32 AZUREGOS = 6109;
constexpr uint32 EMERISS = 14889;
constexpr uint32 TAERAR = 14890;
constexpr uint32 MARK_OF_FROST = 23182;
constexpr uint32 MARK_OF_NATURE = 25040;
constexpr uint32 REFLECT = 22067;
constexpr uint32 VOLATILE_INFECTION = 24928;
constexpr uint32 DREAM_FOG = 15224;
constexpr uint32 HAZARD_TRIGGER = 12999;

uint32 CoaDragonBlock(uint32 entry)
{
    switch (entry)
    {
        case 14887: return 2119400;
        case 14890: return 2119500;
        case 14888: return 2119600;
        case 14889: return 2119700;
        default: return 0;
    }
}

bool KnownScript(Creature const* boss)
{
    std::string expected;
    switch (boss->GetEntry())
    {
        case 6109: expected = "boss_azuregos"; break;
        case 14887: expected = "boss_ysondre"; break;
        case 14888: expected = "boss_lethon"; break;
        case 14889: expected = "boss_emeriss"; break;
        case 14890: expected = "boss_taerar"; break;
        default: return false;
    }
    auto const& script = sObjectMgr->GetScriptName(boss->GetCreatureTemplate()->ScriptID);
    return script == expected || script == expected + "_coa";
}

class AddAttack : public AttackAction
{
public:
    explicit AddAttack(PlayerbotAI* ai) : AttackAction(ai, "world boss add") {}
    bool Start(Unit* target) { return Attack(target); }
};

struct OwnedAddCheck
{
    Creature* boss;
    Player* player;
    bool operator()(Creature* creature) const
    {
        return creature && creature->IsAlive() && creature->IsSummon() && creature->GetEntry() != DREAM_FOG &&
            creature->GetEntry() != HAZARD_TRIGGER &&
            creature->ToTempSummon()->GetSummonerGUID() == boss->GetGUID() &&
            !creature->GetCharmerOrOwnerPlayerOrPlayerItself() && player->IsValidAttackTarget(creature) &&
            player->IsWithinDistInMap(creature, 100.0f);
    }
};

Creature* OwnedAdd(Creature* boss, Player* player)
{
    if (!boss || !player || boss->GetMap() != player->GetMap())
        return nullptr;
    std::list<Creature*> found;
    OwnedAddCheck check{boss, player};
    Acore::CreatureListSearcher<OwnedAddCheck> searcher(player, found, check);
    Cell::VisitObjects(player, searcher, 100.0f);
    Creature* chosen = nullptr;
    for (Creature* add : found)
        if (!chosen || player->GetExactDist(add) < player->GetExactDist(chosen))
            chosen = add;
    return chosen;
}

Creature* BossOnMap(PlayerbotAI* ai, ActivityMemberView const& row)
{
    Unit* const target = row.objective.IsEmpty() ? nullptr : ai->GetUnit(row.objective);
    Creature* const creature = target ? target->ToCreature() : nullptr;
    if (!creature)
        return nullptr;
    if (CampaignRules::SupportedBoss(creature->GetEntry()))
        return creature;
    TempSummon* const add = creature->ToTempSummon();
    Creature* const boss = add ? add->GetSummonerCreatureBase() : nullptr;
    return boss && CampaignRules::SupportedBoss(boss->GetEntry()) && boss->GetMap() == ai->GetBot()->GetMap()
        ? boss : nullptr;
}

bool Marked(Player* bot, uint32 bossEntry)
{
    if (bossEntry == AZUREGOS)
        return bot->HasAura(MARK_OF_FROST) || bot->HasAura(2119117) || bot->HasAura(2119116);
    uint32 const block = CoaDragonBlock(bossEntry);
    return bot->HasAura(MARK_OF_NATURE) || (block && (bot->HasAura(block + 12) || bot->HasAura(block + 13)));
}

ActivityMemberView const* ActiveMember(PlayerbotAI* ai)
{
    if (!ai || !ai->GetBot() || !ActivityRegistry::AnyHeld())
        return nullptr;
    ActivityMemberView const* const row = CoaActivityBot::Refresh(ai).row;
    return row && row->kind == ActivityKindId::WorldBoss && row->phase == ActivityPhase::Active ? row : nullptr;
}

bool MarkedFor(PlayerbotAI* ai, ActivityMemberView const& row)
{
    Player* const bot = ai->GetBot();
    if (Creature* const boss = BossOnMap(ai, row))
        return Marked(bot, boss->GetEntry());
    if (CampaignRules::SupportedBoss(row.objective.GetEntry()))
        return Marked(bot, row.objective.GetEntry());
    return Marked(bot, AZUREGOS) || Marked(bot, 14887) || Marked(bot, 14888) ||
           Marked(bot, EMERISS) || Marked(bot, TAERAR);
}

void StopNegativeCast(Player* bot)
{
    for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL})
        if (Spell* const current = bot->GetCurrentSpell(type); current && !current->GetSpellInfo()->IsPositive())
            bot->InterruptSpell(type, false);
}

void StopMarked(PlayerbotAI* ai)
{
    Player* const bot = ai->GetBot();
    StopNegativeCast(bot);
    bot->AttackStop();
    AiObjectContext* const context = ai->GetAiObjectContext();
    context->GetValue<Unit*>("current target")->Set(nullptr);
    context->GetValue<Unit*>("old target")->Set(nullptr);
    context->GetValue<GuidVector>("prioritized targets")->Set({});
    context->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    context->GetValue<ObjectGuid>("pull strategy target")->Set(ObjectGuid::Empty);
    bot->SetSelection(ObjectGuid::Empty);
    bot->GetMotionMaster()->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
    if (bot->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == NULL_MOTION_TYPE &&
        !bot->IsInFlight() && !bot->GetVehicle())
        bot->StopMoving();
}
}  // namespace

CampaignSettings WorldBossKind::Settings(ActivityConfig const& config) const
{
    CampaignSettings s;
    s.enabled = config.worldBossEnable;
    s.minimum = config.worldBossMinBots;
    s.maximum = config.worldBossMaxBots;
    s.recruitSeconds = config.worldBossRecruitSeconds;
    s.minutes = config.worldBossMinutes;
    s.raid = true;
    s.lowest = s.highest = 60;
    return s;
}

std::vector<ActivityCampaignTarget> const& WorldBossKind::Configured(ActivityConfig const& config) const
{
    return config.worldBossTargets;
}

bool WorldBossKind::Validate(ActivityServices& services, ActivityStartRequest const& request,
                             ActivityCampaignTarget const& target, CampaignState& state, std::string& why) const
{
    if (!CampaignWorld::ResolveTarget(target, state.spawn, why))
        return false;
    if (!CampaignRules::SupportedBoss(state.spawn.entry) || target.strategy != "coa_worldboss" ||
        target.rally.mapId != state.spawn.point.mapId)
    {
        why = "world boss requires an Azuregos or emerald dragon spawn and the coa_worldboss tactics";
        return false;
    }
    Creature* const boss = ActivityGuards::FindSpawn(target.spawnId);
    if (!boss || !KnownScript(boss) || !boss->IsAlive() || boss->IsInCombat() || boss->GetLootRecipient() ||
        boss->IsDuringRemoveFromWorld() || ActivityGuards::QuestTargetNeeded(state.spawn.entry,
                                                                           services.realPlayers, nullptr))
    {
        why = "world boss must be loaded, alive, unclaimed, and not required by a real player's quest";
        return false;
    }
    if (!request.gm && (!services.config.worldBossGmKillVerified || CampaignRules::NeedsHistory(false,
        ActivityDirector::Instance().CleanRuns(ActivityKindId::WorldBoss), services.config.worldBossCleanRunsGate)))
    {
        why = "ambient world bosses require a verified GM kill and an actual completed clean boss attempt";
        return false;
    }
    return true;
}

bool WorldBossKind::Prepare(ActivityServices&, Activity& activity, CampaignState& state)
{
    if (!CampaignWorld::Roles(activity, 1, 2))
    {
        activity.Finish(ActivityOutcome::TooFew, "world boss needs an actual tank and two actual healers");
        return false;
    }
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot)
            if (Player* const bot = ActivityWorld::FindMember(member); bot && Marked(bot, state.spawn.entry))
            {
                activity.Finish(ActivityOutcome::Stopped, "a bot still carries this boss's native death mark");
                return false;
            }
    return true;
}

void WorldBossKind::Encounter(ActivityServices& services, Activity& activity, CampaignState& state)
{
    Creature* const boss = CampaignWorld::SpawnFor(activity, state);
    Player* const leader = activity.Leader() ? ActivityWorld::FindMember(*activity.Leader()) : nullptr;
    if (!boss || !leader || !boss->IsAlive())
    {
        CampaignKind::Encounter(services, activity, state);
        return;
    }
    if (!CampaignWorld::Roles(activity, 1, 2))
    {
        activity.Finish(ActivityOutcome::TooFew, "world boss lost its actual tank or healer coverage");
        return;
    }
    if (boss->GetEntry() == TAERAR && boss->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE))
    {
        if (!state.engaged || CampaignWorld::OutsiderEngaged(activity, boss))
        {
            activity.Finish(ActivityOutcome::Stopped, "Taerar's banished attempt is not owned by this group");
            return;
        }
        Creature* const shade = OwnedAdd(boss, leader);
        activity.SetObjective(shade ? shade->GetGUID() : ObjectGuid::Empty);
        activity.SetPhase(ActivityPhase::Active, services.nowMs);
        return;  // Native script removes banish when its actual shades die; no NPC flags are changed here.
    }
    CampaignKind::Encounter(services, activity, state);
}

bool WorldBossTactics::AttackAllowed(PlayerbotAI* ai, Unit* target)
{
    ActivityMemberView const* const row = ActiveMember(ai);
    return !target || !row || !MarkedFor(ai, *row);
}

bool WorldBossTactics::MovementAllowed(PlayerbotAI* ai)
{
    ActivityMemberView const* const row = ActiveMember(ai);
    return !row || !ai->GetBot()->IsAlive() || !MarkedFor(ai, *row);
}

Unit* WorldBossTactics::ObjectiveTarget(PlayerbotAI* ai, Unit* proposed)
{
    ActivityMemberView const* const row = ActiveMember(ai);
    if (!row || MarkedFor(ai, *row))
        return row ? nullptr : proposed;
    Creature* const boss = BossOnMap(ai, *row);
    Player* const bot = ai->GetBot();
    Unit* const current = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
    Creature* const add = current ? current->ToCreature() : nullptr;
    if (boss && add && add->GetMap() == bot->GetMap() && add->IsInWorld() &&
        !add->IsDuringRemoveFromWorld() && OwnedAddCheck{boss, bot}(add))
        return add;
    return proposed;
}

bool WorldBossTactics::CastAllowed(Unit* caster, SpellInfo const* spell, Unit* target)
{
    if (!caster || !spell || spell->IsPositive() || !ActivityRegistry::AnyHeld())
        return true;
    Player* const player = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!player || player->GetMap() != caster->GetMap())
        return true;
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(player);
    ActivityMemberView const* const row = ActiveMember(ai);
    if (!row)
        return true;
    if (MarkedFor(ai, *row))
        return false;
    if (spell->DmgClass != SPELL_DAMAGE_CLASS_MAGIC)
        return true;
    Creature* const boss = BossOnMap(ai, *row);
    return CampaignRules::ReflectCastAllowed(boss && boss->GetEntry() == AZUREGOS && boss->HasAura(REFLECT),
        true, !target || target == boss);  // Untargeted magic area attacks can also hit the reflecting boss.
}

void WorldBossTactics::Update(PlayerbotAI* ai, uint64 nowMs)
{
    if (!ai || !ActivityRegistry::AnyHeld())
        return;
    ActivityBotState& state = CoaActivityBot::Refresh(ai);
    ActivityMemberView const* const row = state.row;
    if (!row || row->kind != ActivityKindId::WorldBoss || row->phase != ActivityPhase::Active)
        return;
    Player* const bot = ai->GetBot();
    if (!bot || !bot->IsAlive())
        return;
    if (MarkedFor(ai, *row))
    {
        StopMarked(ai);
        return;
    }
    Creature* const boss = BossOnMap(ai, *row);
    if (!boss || !boss->IsAlive() || boss->GetMap() != bot->GetMap() ||
        bot->GetExactDist(boss) > 100.0f)
        return;
    if (boss->GetEntry() == AZUREGOS && boss->HasAura(REFLECT))
        StopNegativeCast(bot);
    if (nowMs < state.worldBossTacticMs)
        return;
    state.worldBossTacticMs = nowMs + 2000;
    if (bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED) ||
        bot->IsBeingTeleported() || bot->IsInFlight() || bot->GetVehicle())
        return;
    // Boss-owned adds are actual native summons: do not borrow another encounter's nearby creatures.
    if (Creature* const add = OwnedAdd(boss, bot); add && !PlayerbotAI::IsHeal(bot))
    {
        if (AddAttack(ai).Start(add))
            ai->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({add->GetGUID()});
        return;
    }
    if (PlayerbotAI::IsTank(bot) && boss->GetVictim() == bot)
        return;
    float const side = (bot->GetGUID().GetCounter() & 1) ? 1.0f : -1.0f;
    float const offset = 0.08f * float((bot->GetGUID().GetCounter() / 2) % 5);
    // Front-side flanks avoid the 90-degree breath cone and CoA's entire rear-half tail sweep.
    float const angle = boss->GetOrientation() + side * (1.05f + offset);
    bool const ranged = PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot);
    float const distance = boss->GetEntry() == EMERISS &&
        (bot->HasAura(VOLATILE_INFECTION) || bot->HasAura(2119722)) ? 35.0f
        : ranged ? 25.0f : 8.0f;
    float x = 0, y = 0, z = 0;
    boss->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), distance, angle);
    // A native fog is dangerous even when it cannot be attacked. Move to the opposite flank when one is close.
    std::list<Creature*> fogs;
    bot->GetCreatureListWithEntryInGrid(fogs, DREAM_FOG, 8.0f);
    bot->GetCreatureListWithEntryInGrid(fogs, HAZARD_TRIGGER, 8.0f);
    if (!fogs.empty())
        boss->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), std::max(33.0f, distance + 8.0f), angle + side * 0.6f);
    if (!Acore::IsValidMapCoord(x, y, z) || std::abs(z - bot->GetPositionZ()) > 10.0f ||
        !bot->IsWithinLOS(x, y, z) || bot->GetExactDist(x, y, z) <= 4.0f)
        return;
    StopNegativeCast(bot);
    bot->GetMotionMaster()->MovePoint(0, x, y, z);
}

std::unique_ptr<ActivityKind> CreateWorldBossKind() { return std::make_unique<WorldBossKind>(); }
