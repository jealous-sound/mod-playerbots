/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#include "ActivityPvpPolicy.h"
#include "ActivityRegistry.h"
#include "AiObjectContext.h"
#include "CoaPlayerPrefs.h"
#include "CoaTime.h"
#include "Creature.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldSession.h"
#include "TemporarySummon.h"
#include "Map.h"
#include <map>
#include <mutex>

namespace
{
struct Hit
{
    uint32 activity = 0;
    uint64 atMs = 0;
    bool dead = false;
};
std::mutex hitLock;
std::map<ObjectGuid, Hit> hits;
std::optional<ActivityMemberView> Row(PlayerbotAI* ai)
{
    if (!ai || !ActivityRegistry::AnyHeld())
        return std::nullopt;
    auto const row = ActivityRegistry::Find(ai->GetBot()->GetGUID().GetCounter());
    return row && row->epoch == ai->GetCoaEpoch() && ActivityPvpPolicy::Controlled(row->kind) && row->pvpRuleset ? row : std::nullopt;
}
Player* Owner(Unit* unit)
{
    if (!unit)
        return nullptr;
    Player* owner = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
    Creature* const creature = unit->ToCreature();
    if (!owner && creature && creature->IsSummon())
        if (Unit* const summoner = creature->ToTempSummon()->GetSummonerUnit())
            owner = summoner->GetCharmerOrOwnerPlayerOrPlayerItself();
    return owner;
}

bool Ready(Player* bot, uint32 ruleset, bool applied)
{
    return bot && bot->IsInWorld() && bot->IsAlive() && bot->GetMap() && !bot->GetMap()->Instanceable() &&
        !bot->duel && !bot->IsInFlight() && !bot->GetVehicle() && !bot->IsBeingTeleported() &&
        !bot->InBattleground() && !bot->InArena() && !bot->HasAura(ActivityThreshold::SPELL_MERCENARY) &&
        (!applied || (bot->IsPvP() && !bot->HasAura(ActivityThreshold::SPELL_PVE) && ruleset && bot->HasAura(ruleset)));
}

uint32 Ruleset(Player* player)
{
    return player->HasAura(ActivityThreshold::SPELL_HIGH_RISK) ? ActivityThreshold::SPELL_HIGH_RISK :
           player->HasAura(ActivityThreshold::SPELL_WAR_MODE) ? ActivityThreshold::SPELL_WAR_MODE : 0;
}

bool ImplicitCasterOnly(SpellInfo const* spell)
{
    // InitExplicitTargets removes a redundant unit target before CanPrepare for caster-only auras.
    if (spell->GetExplicitTargetMask() & (TARGET_FLAG_UNIT_MASK | TARGET_FLAG_CORPSE_MASK) ||
        spell->IsAffectingArea())
        return false;
    bool effectFound = false;
    for (SpellEffectInfo const& effect : spell->Effects)
        if (effect.IsEffect())
        {
            effectFound = true;
            if (effect.TargetA.GetTarget() != TARGET_UNIT_CASTER ||
                (effect.TargetB.GetTarget() != 0 && effect.TargetB.GetTarget() != TARGET_UNIT_CASTER))
                return false;
        }
    return effectFound;
}
}

bool ActivityPvpPolicy::Controlled(ActivityKindId kind)
{
    return kind == ActivityKindId::PvpGang || kind == ActivityKindId::Bounty || kind == ActivityKindId::HubDefense ||
           kind == ActivityKindId::Frontline;
}

bool ActivityPvpPolicy::Active(PlayerbotAI* ai) { return Row(ai).has_value(); }

bool ActivityPvpPolicy::Eligible(Player* attacker, Player* target, uint32 ruleset, bool nativeCheck)
{
    if (!Ready(attacker, ruleset, nativeCheck) || !target || attacker == target || !target->IsInWorld() ||
        attacker->GetMap() != target->GetMap() || !target->IsAlive() || target->IsGameMaster() ||
        attacker->GetTeamId() == target->GetTeamId() || target->duel || target->IsInFlight() ||
        target->GetVehicle() || target->IsBeingTeleported() || target->InBattleground() || target->InArena() ||
        !attacker->InSamePhase(target) || !target->IsPvP() || target->HasAura(ActivityThreshold::SPELL_PVE) ||
        target->HasAura(ActivityThreshold::SPELL_MERCENARY) || !ruleset || Ruleset(target) != ruleset)
        return false;
    if (!target->GetSession())
        return false;
    if (!target->GetSession()->IsBot())
    {
        auto const& prefs = CoaPlayerPrefs::Instance();
        if (!prefs.IsLoaded() || (prefs.Get(target->GetSession()->GetAccountId()).activityMask & COA_OPTOUT_HOSTILE))
            return false;
    }
    return !nativeCheck || attacker->IsValidAttackTarget(target);
}

bool ActivityPvpPolicy::AttackAllowed(PlayerbotAI* ai, Unit* target)
{
    auto const row = Row(ai);
    if (!row)
        return true;
    Player* const bot = ai->GetBot();
    if (!Ready(bot, row->pvpRuleset, true) || !target || target->GetMap() != bot->GetMap())
        return false;
    Player* const owner = Owner(target);
    if (owner)
        return Eligible(bot, owner, row->pvpRuleset) && bot->IsValidAttackTarget(target);
    // Uncontrolled creatures are defensive targets only; quest givers are never objectives.
    Creature* const creature = target->ToCreature();
    if (row->kind == ActivityKindId::Frontline && creature && row->objective == creature->GetGUID() &&
        creature->GetLevel() <= 60 && bot->IsValidAttackTarget(creature))
        return true;
    Unit* const victim = creature ? creature->GetVictim() : nullptr;
    Player* const attacked = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    auto const attackedRow = attacked ? ActivityRegistry::Find(attacked->GetGUID().GetCounter()) : std::nullopt;
    return creature && !creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER) && attackedRow &&
           attackedRow->activityId == row->activityId && bot->IsValidAttackTarget(creature);
}

bool ActivityPvpPolicy::CastAllowed(PlayerbotAI* ai, SpellInfo const* spell, Unit* target)
{
    auto const row = Row(ai);
    if (!row)
        return true;
    if (!spell)
        return false;
    if (spell->Id == row->pvpRuleset &&
        (target == ai->GetBot() || (!target && ImplicitCasterOnly(spell))) &&
        Ready(ai->GetBot(), row->pvpRuleset, false))
        return true;
    if (!Ready(ai->GetBot(), row->pvpRuleset, true))
        return false;
    if (spell->IsPositive())
        return !target || !target->GetCharmerOrOwnerPlayerOrPlayerItself() ||
               target->GetCharmerOrOwnerPlayerOrPlayerItself()->GetTeamId() == ai->GetBot()->GetTeamId();
    return !spell->IsAffectingArea() && target && AttackAllowed(ai, target);
}

bool ActivityPvpPolicy::CanPrepare(Spell* spell)
{
    if (!spell || !spell->GetCaster())
        return true;
    Player* const owner = Owner(spell->GetCaster());
    PlayerbotAI* const ai = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    return !ai || CastAllowed(ai, spell->GetSpellInfo(), spell->m_targets.GetUnitTarget());
}

void ActivityPvpPolicy::Update(PlayerbotAI* ai)
{
    if (!Row(ai))
        return;
    Player* const bot = ai->GetBot();
    if (Unit* const victim = bot->GetVictim(); victim && !AttackAllowed(ai, victim))
    {
        bot->AttackStop();
        bot->InterruptNonMeleeSpells(false);
        ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(nullptr);
    }
    for (Unit* const controlled : bot->m_Controlled)
        if (controlled && controlled->GetVictim() && !AttackAllowed(ai, controlled->GetVictim()))
        {
            controlled->AttackStop();
            controlled->InterruptNonMeleeSpells(false);
        }
}

void ActivityPvpPolicy::OnDamage(Unit* attacker, Unit* victim, uint32& damage)
{
    if (!damage || !victim)
        return;
    Player* const owner = Owner(attacker);
    PlayerbotAI* const ai = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    auto const row = Row(ai);
    if (row && !AttackAllowed(ai, victim))
    {
        damage = 0;
        return;
    }
}

void ActivityPvpPolicy::OnDeath(Unit* victim, Unit* killer)
{
    if (!victim || !victim->ToPlayer() || !killer)
        return;
    Player* const owner = Owner(killer);
    PlayerbotAI* const ai = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    auto const row = Row(ai);
    if (!row)
        return;
    std::lock_guard<std::mutex> lock(hitLock);
    if (hits.size() < 64 || hits.count(victim->GetGUID()))
        hits[victim->GetGUID()] = {row->activityId, CoaTime::NowMs(), true};
}

bool ActivityPvpPolicy::Killed(uint32 activityId, ObjectGuid victim)
{
    std::lock_guard<std::mutex> lock(hitLock);
    auto const found = hits.find(victim);
    return found != hits.end() && found->second.activity == activityId && found->second.dead;
}

void ActivityPvpPolicy::Release(uint32 activityId)
{
    std::lock_guard<std::mutex> lock(hitLock);
    for (auto it = hits.begin(); it != hits.end();)
        if (it->second.activity == activityId)
            it = hits.erase(it);
        else
            ++it;
}
