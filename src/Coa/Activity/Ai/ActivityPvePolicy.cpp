/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityPvePolicy.h"
#include "ActivityRegistry.h"
#include "ActivityPvpPolicy.h"
#include "FestivalVisit.h"
#include "GoldRoll.h"
#include "PlayerService.h"
#include "LeaderScaler.h"
#include "AiObjectContext.h"
#include "CoaActivityValues.h"
#include "Creature.h"
#include "CreatureData.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "Spell.h"
#include "TemporarySummon.h"
#include "WorldBoss.h"
#include "WorldSession.h"
#include <algorithm>

namespace
{
ActivityKindId Kind(PlayerbotAI* ai)
{
    if (!ai || !ai->GetBot() || !ActivityRegistry::AnyHeld())
        return ActivityKindId::None;
    auto const row = ActivityRegistry::Find(ai->GetBot()->GetGUID().GetCounter());
    return row && row->epoch == ai->GetCoaEpoch() && !row->pvpRuleset ? row->kind : ActivityKindId::None;
}

bool Safe(PlayerbotAI* ai)
{
    Player* const bot = ai->GetBot();
    bool const instance = Kind(ai) == ActivityKindId::PugRaid;
    bool const capital = Kind(ai) == ActivityKindId::LeaderEncounter || Kind(ai) == ActivityKindId::CapitalRaid ||
                         Kind(ai) == ActivityKindId::LieutenantRaid || Kind(ai) == ActivityKindId::Frontline;
    bool const ordinaryCapitalFlag = capital && bot->IsInWorld() && bot->GetMap() && !bot->GetMap()->Instanceable() &&
        bot->IsAlive() && !bot->GetVehicle() && !bot->IsInFlight() && !bot->duel && !bot->IsBeingTeleported() &&
        !bot->IsFFAPvP() && !bot->HasAura(ActivityThreshold::SPELL_HIGH_RISK) &&
        !bot->HasAura(ActivityThreshold::SPELL_MERCENARY) && !bot->HasAura(ActivityThreshold::SPELL_WAR_MODE) &&
        !bot->InBattleground() && !bot->InArena() && bot->HasAura(ActivityThreshold::SPELL_PVE);
    if (!ActivityPvePolicy::SafePlayer(bot, instance) && !ordinaryCapitalFlag)
        return false;
    Player* const master = ai->GetMaster();
    if (!master)
        return true;
    bool const capitalMaster = capital && master->GetSession() && master->GetSession()->IsBot() &&
                              master->HasAura(ActivityThreshold::SPELL_PVE) && !master->IsFFAPvP() &&
                              !master->HasAura(ActivityThreshold::SPELL_WAR_MODE) &&
                              !master->HasAura(ActivityThreshold::SPELL_HIGH_RISK) &&
                              !master->HasAura(ActivityThreshold::SPELL_MERCENARY) && !master->duel &&
                              !master->IsInFlight() && !master->GetVehicle();
    return master->GetMap() == bot->GetMap() && master->IsAlive() &&
           (ActivityPvePolicy::SafePlayer(master, instance) || capitalMaster);
}

Player* Owner(Creature* creature)
{
    Player* owner = creature->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!owner && creature->IsSummon())
        if (Unit* const summoner = creature->ToTempSummon()->GetSummonerUnit())
            owner = summoner->GetCharmerOrOwnerPlayerOrPlayerItself();
    return owner;
}

void Guard(PlayerbotAI* ai, Creature* creature, ActivityBotState& state)
{
    if (!creature || !creature->GetMap() || creature->GetMap() != ai->GetBot()->GetMap() ||
        Owner(creature) != ai->GetBot())
        return;
    auto const found = std::find_if(state.m7ControlledReact.begin(), state.m7ControlledReact.end(),
        [creature](auto const& entry) { return entry.guid == creature->GetGUID(); });
    if (found == state.m7ControlledReact.end())
        state.m7ControlledReact.push_back({creature->GetGUID(), uint8(creature->GetReactState()),
                                          creature->IsAIEnabled});
    creature->IsAIEnabled = false;
    creature->SetReactState(REACT_PASSIVE);
    creature->AttackStop();
    creature->InterruptNonMeleeSpells(false);
    ai->SetM7ControlledGuard(true);
}

bool Reflection(SpellInfo const* spell)
{
    if (!spell)
        return false;
    for (SpellEffectInfo const& effect : spell->Effects)
        if (effect.ApplyAuraName == SPELL_AURA_REFLECT_SPELLS ||
            effect.ApplyAuraName == SPELL_AURA_REFLECT_SPELLS_SCHOOL || effect.ApplyAuraName == SPELL_AURA_DAMAGE_SHIELD)
            return true;
    return false;
}
}

bool ActivityPvePolicy::Protected(ActivityKindId kind)
{
    return kind == ActivityKindId::Escort || kind == ActivityKindId::RareScout ||
           kind == ActivityKindId::LeaderEncounter || kind == ActivityKindId::CapitalRaid ||
           kind == ActivityKindId::LieutenantRaid || kind == ActivityKindId::Frontline ||
           kind == ActivityKindId::TownDefense || kind == ActivityKindId::PugRaid ||
           kind == ActivityKindId::WorldBoss || kind == ActivityKindId::PlayerService ||
           kind == ActivityKindId::FestivalVisit || kind == ActivityKindId::GoldRoll ||
           kind == ActivityKindId::RareCamp || kind == ActivityKindId::HubDefense;
}

bool ActivityPvePolicy::TargetAllowed(ActivityKindId kind, bool safe, bool playerControlled, uint32 creatureRank)
{
    if (!Protected(kind))
        return true;
    return safe && !playerControlled && (kind != ActivityKindId::RareScout ||
        (creatureRank != CREATURE_ELITE_RARE && creatureRank != CREATURE_ELITE_RAREELITE));
}

bool ActivityPvePolicy::SpellAllowed(ActivityKindId kind, bool safe, bool positive, bool area, bool targetAllowed)
{
    return !Protected(kind) || (safe && (positive || (!area && targetAllowed)));
}

bool ActivityPvePolicy::SafePlayer(Player* player, bool allowInstance)
{
    return player && player->IsInWorld() && player->GetMap() && (allowInstance || !player->GetMap()->Instanceable()) &&
        player->IsAlive() && !player->GetVehicle() && !player->IsInFlight() && !player->duel &&
        !player->IsBeingTeleported() && !player->IsPvP() &&
        !player->IsFFAPvP() && !player->HasAura(ActivityThreshold::SPELL_HIGH_RISK) &&
        !player->HasAura(ActivityThreshold::SPELL_MERCENARY) && !player->HasAura(ActivityThreshold::SPELL_WAR_MODE) &&
        !player->InBattleground() && !player->InArena();
}

bool ActivityPvePolicy::RareSafe(Player* player)
{
    return SafePlayer(player) && !player->HasAuraType(SPELL_AURA_REFLECT_SPELLS) &&
        !player->HasAuraType(SPELL_AURA_REFLECT_SPELLS_SCHOOL) && !player->HasAuraType(SPELL_AURA_DAMAGE_SHIELD);
}

bool ActivityPvePolicy::AttackAllowed(PlayerbotAI* ai, Unit* target)
{
    if (!ActivityPvpPolicy::AttackAllowed(ai, target) || !LeaderScaler::AttackAllowed(ai, target))
        return false;
    ActivityKindId const kind = Kind(ai);
    if (!Protected(kind))
        return true;
    if (kind == ActivityKindId::WorldBoss && !WorldBossTactics::AttackAllowed(ai, target))
        return false;
    if (!target || target->GetMap() != ai->GetBot()->GetMap())
        return false;
    if (kind == ActivityKindId::FestivalVisit || kind == ActivityKindId::GoldRoll)
        return false;
    if (kind == ActivityKindId::LeaderEncounter || kind == ActivityKindId::CapitalRaid ||
        kind == ActivityKindId::LieutenantRaid)
    {
        auto const row = ActivityRegistry::Find(ai->GetBot()->GetGUID().GetCounter());
        if (row && row->Has(ACTIVITY_VIEW_RETREAT))
            return false;
    }
    if (kind == ActivityKindId::RareCamp)
    {
        auto const row = ActivityRegistry::Find(ai->GetBot()->GetGUID().GetCounter());
        if (row && row->Has(ACTIVITY_VIEW_RETREAT))
            return false;
        Creature* const rare = row ? ai->GetBot()->GetMap()->GetCreature(row->objective) : nullptr;
        if (rare)
            for (auto const& reference : rare->GetMap()->GetPlayers())
            {
                Player* const player = reference.GetSource();
                if (player && player->GetSession() && !player->GetSession()->IsBot() &&
                    player->IsWithinDistInMap(rare, 200))
                    return false;
            }
    }
    Creature const* const creature = target->ToCreature();
    uint32 const rank = creature ? creature->GetCreatureTemplate()->rank : 0;
    Player* const targetOwner = target->GetCharmerOrOwnerPlayerOrPlayerItself() ?
                                target->GetCharmerOrOwnerPlayerOrPlayerItself() :
                                target->ToCreature() ? Owner(target->ToCreature()) : nullptr;
    return TargetAllowed(kind, Safe(ai), targetOwner != nullptr, rank);
}

bool ActivityPvePolicy::CastAllowed(PlayerbotAI* ai, SpellInfo const* spell, Unit* target)
{
    if (!ActivityPvpPolicy::CastAllowed(ai, spell, target))
        return false;
    ActivityKindId const kind = Kind(ai);
    if (!Protected(kind))
        return true;
    if (!spell)
        return false;
    if (kind == ActivityKindId::WorldBoss && !WorldBossTactics::CastAllowed(ai->GetBot(), spell, target))
        return false;
    if (kind == ActivityKindId::FestivalVisit && Safe(ai) && FestivalVisitActions::AllowsCast(ai, spell, target))
        return true;
    if (kind == ActivityKindId::RareScout && (Reflection(spell) || !RareSafe(ai->GetBot())))
        return false;
    for (SpellEffectInfo const& effect : spell->Effects)
        if (effect.Effect == SPELL_EFFECT_SUMMON || effect.Effect == SPELL_EFFECT_SUMMON_PET ||
            effect.Effect == SPELL_EFFECT_CREATE_TAMED_PET || effect.Effect == SPELL_EFFECT_RESURRECT_PET ||
            effect.Effect == SPELL_EFFECT_CALL_PET || effect.Effect == SPELL_EFFECT_SUMMON_PLAYER ||
            effect.Effect == SPELL_EFFECT_SUMMON_RAF_FRIEND)
            return false;
    bool const positive = spell->IsPositive();
    if (!positive && (kind == ActivityKindId::LeaderEncounter || kind == ActivityKindId::CapitalRaid ||
        kind == ActivityKindId::LieutenantRaid))
    {
        auto const row = ActivityRegistry::Find(ai->GetBot()->GetGUID().GetCounter());
        if (row && row->Has(ACTIVITY_VIEW_RETREAT))
            return false;
    }
    bool safe = Safe(ai);
    if (positive && target)
    {
        Player* const player = target->GetCharmerOrOwnerPlayerOrPlayerItself() ?
                               target->GetCharmerOrOwnerPlayerOrPlayerItself() :
                               target->ToCreature() ? Owner(target->ToCreature()) : nullptr;
        PlayerbotAI* const friendlyAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
        auto const mine = ActivityRegistry::Find(ai->GetBot()->GetGUID().GetCounter());
        auto const friendly = player ? ActivityRegistry::Find(player->GetGUID().GetCounter()) : std::nullopt;
        bool const sameCapital = friendlyAI && mine && friendly && mine->activityId == friendly->activityId &&
            friendly->epoch == friendlyAI->GetCoaEpoch() &&
            (kind == ActivityKindId::LeaderEncounter || kind == ActivityKindId::CapitalRaid ||
             kind == ActivityKindId::LieutenantRaid || kind == ActivityKindId::Frontline) && Safe(friendlyAI);
        safe = safe && target->GetMap() == ai->GetBot()->GetMap() &&
               (!player || SafePlayer(player, kind == ActivityKindId::PugRaid) || sameCapital);
    }
    return SpellAllowed(kind, safe, positive, spell->IsAffectingArea(), target && AttackAllowed(ai, target));
}

void ActivityPvePolicy::Update(PlayerbotAI* ai)
{
    ActivityKindId const kind = Kind(ai);
    if (!Protected(kind) && !ai->HasM7ControlledGuard())
        return;
    Player* const bot = ai->GetBot();
    if (Protected(kind))
    {
        Enroll(ai, kind);
        if (Unit* const victim = bot->GetVictim(); victim && !AttackAllowed(ai, victim))
        {
            bot->AttackStop();
            bot->InterruptNonMeleeSpells(false);
            ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
        }
    }
    else
        Release(ai);
}

void ActivityPvePolicy::Enroll(PlayerbotAI* ai, ActivityKindId kind)
{
    if (!ai || !Protected(kind))
        return;
    ActivityBotState& state = CoaActivityBot::Refresh(ai);
    Player* const bot = ai->GetBot();
    if (!ai->HasM7ControlledGuard())
    {
        bot->InterruptNonMeleeSpells(false);
        bot->AttackStop();
        ai->SetM7ControlledGuard(true);
    }
    state.m7ControlledReact.erase(std::remove_if(state.m7ControlledReact.begin(), state.m7ControlledReact.end(),
        [bot](auto const& entry)
        {
            Creature* const creature = ObjectAccessor::GetCreature(*bot, entry.guid);
            return !creature || Owner(creature) != bot;
        }), state.m7ControlledReact.end());
    for (Unit* const unit : bot->m_Controlled)
        Guard(ai, unit ? unit->ToCreature() : nullptr, state);
}

void ActivityPvePolicy::Release(PlayerbotAI* ai)
{
    if (!ai || !ai->GetBot() || !ai->HasM7ControlledGuard())
        return;
    ActivityBotState& state = CoaActivityBot::Refresh(ai);
    Player* const bot = ai->GetBot();
    if (!bot->GetMap())
        return;
    for (auto const& entry : state.m7ControlledReact)
    {
        Creature* const creature = ObjectAccessor::GetCreature(*bot, entry.guid);
        if (!creature || creature->GetMap() != bot->GetMap() || Owner(creature) != bot)
            continue;
        creature->IsAIEnabled = entry.aiEnabled;
        creature->SetReactState(ReactStates(entry.react));
    }
    FestivalVisitActions::Release(ai);
    GoldRollActions::Release(ai);
    PlayerServiceCatalog::Release(ai);
    state.m7ControlledReact.clear();
    ai->SetM7ControlledGuard(false);
}

void ActivityPvePolicy::OnCreatureAddWorld(Creature* creature)
{
    if (!creature || !ActivityRegistry::AnyHeld())
        return;
    Player* const owner = Owner(creature);
    if (!owner || owner->GetMap() != creature->GetMap())
        return;
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(owner);
    if (!ai || !Protected(Kind(ai)))
        return;
    ActivityBotState& state = CoaActivityBot::Refresh(ai);
    Guard(ai, creature, state);
}

void ActivityPvePolicy::PreparePetSave(PlayerbotAI* ai)
{
    if (!ai || !ai->HasM7ControlledGuard())
        return;
    Pet* const pet = ai->GetBot()->GetPet();
    if (!pet || pet->GetMap() != ai->GetBot()->GetMap())
        return;
    ActivityBotState const& state = CoaActivityBot::Refresh(ai);
    auto const found = std::find_if(state.m7ControlledReact.begin(), state.m7ControlledReact.end(),
        [pet](auto const& entry) { return entry.guid == pet->GetGUID(); });
    if (found != state.m7ControlledReact.end())
        pet->SetReactState(ReactStates(found->react));
}

void ActivityPvePolicy::OnDamage(Unit* attacker, Unit* victim, uint32& damage)
{
    if (!attacker || !victim || !damage || !ActivityRegistry::AnyHeld())
        return;
    Player* owner = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!owner)
        if (Creature* const creature = attacker->ToCreature())
            owner = Owner(creature);
    if (!owner || owner->GetMap() != attacker->GetMap())
        return;
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(owner);
    if (ai && Protected(Kind(ai)) && !AttackAllowed(ai, victim))
        damage = 0;
}

bool ActivityPvePolicy::CanPrepare(Spell* spell)
{
    if (!spell || !ActivityRegistry::AnyHeld())
        return true;
    Unit* const caster = spell->GetCaster();
    Unit* const target = spell->m_targets.GetUnitTarget();
    if (Reflection(spell->GetSpellInfo()) && target && target->IsPlayer())
    {
        PlayerbotAI* const targetAI = GET_PLAYERBOT_AI(target->ToPlayer());
        if (targetAI && Kind(targetAI) == ActivityKindId::RareScout)
            return false;
    }
    Player* owner = caster ? caster->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!owner && caster)
        if (Creature* const creature = caster->ToCreature())
            owner = Owner(creature);
    if (!owner)
        if (Unit* const original = spell->GetOriginalCaster())
            owner = original->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!owner || !caster || owner->GetMap() != caster->GetMap())
        return true;
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(owner);
    return !ai || CastAllowed(ai, spell->GetSpellInfo(), target);
}

void ActivityPvePolicy::GuardReflection(Spell* spell, TargetInfo& target)
{
    if (!spell || target.missCondition != SPELL_MISS_REFLECT || target.reflectResult != SPELL_MISS_NONE ||
        !ActivityRegistry::AnyHeld())
        return;
    Unit* const caster = spell->GetCaster();
    if (!caster)
        return;
    Unit* const reflector = ObjectAccessor::GetUnit(*caster, target.targetGUID);
    Player* owner = reflector ? reflector->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!owner && reflector && reflector->ToCreature())
        owner = Owner(reflector->ToCreature());
    PlayerbotAI* const ai = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    if (ai && !AttackAllowed(ai, caster))
    {
        target.reflectResult = SPELL_MISS_IMMUNE;
        target.damage = 0;
        target.damageBeforeTakenMods = 0;
    }
}

bool ActivityPvePolicy::LootAllowed(PlayerbotAI* ai)
{
    ActivityKindId const kind = Kind(ai);
    return kind != ActivityKindId::RareScout && kind != ActivityKindId::RareCamp &&
           kind != ActivityKindId::FestivalVisit && kind != ActivityKindId::GoldRoll &&
           kind != ActivityKindId::PlayerService && kind != ActivityKindId::PugRaid;
}

std::vector<std::string> ActivityPvePolicy::SelfTest()
{
    std::vector<std::string> failures;
    uint32 checks = 0;
    auto check = [&](bool result, char const* name)
    {
        ++checks;
        if (!result)
            failures.emplace_back(std::string("FAIL M7 PvE: ") + name);
    };
    for (ActivityKindId const kind : {ActivityKindId::Escort, ActivityKindId::RareScout})
    {
        check(!TargetAllowed(kind, true, true, 0), "player or player-controlled target");
        check(!TargetAllowed(kind, false, false, 0), "unsafe bot or master");
        check(TargetAllowed(kind, true, false, 0), "ordinary PvE target");
        check(!SpellAllowed(kind, true, false, true, true), "offensive area spell");
        check(!SpellAllowed(kind, true, false, false, false), "offensive target rejected");
        check(!SpellAllowed(kind, false, true, false, true), "unsafe friendly cast");
        check(SpellAllowed(kind, true, true, true, false), "safe friendly area cast");
        check(SpellAllowed(kind, true, false, false, true), "safe single-target PvE cast");
    }
    check(!TargetAllowed(ActivityKindId::RareScout, true, false, CREATURE_ELITE_RARE), "rare tag rejected");
    check(!TargetAllowed(ActivityKindId::RareScout, true, false, CREATURE_ELITE_RAREELITE), "rare elite tag rejected");
    check(TargetAllowed(ActivityKindId::Escort, true, false, CREATURE_ELITE_RARE), "escort PvE rare allowed");
    for (uint8 value = 0; value < uint8(ActivityKindId::Escort); ++value)
    {
        ActivityKindId const kind = ActivityKindId(value);
        check(TargetAllowed(kind, false, true, CREATURE_ELITE_RARE), "legacy target behavior");
        check(SpellAllowed(kind, false, false, true, false), "legacy cast behavior");
    }
    if (failures.empty())
        failures.emplace_back("PASS M7 PvE policy " + std::to_string(checks) + " checks");
    return failures;
}
