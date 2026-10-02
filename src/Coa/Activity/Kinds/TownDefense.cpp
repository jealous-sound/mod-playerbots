/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#include "TownDefense.h"
#include "ActivityDirector.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DataMap.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include <algorithm>
#include <cmath>

namespace
{
bool DeathItemsAllowed(bool ownedWave, std::array<SpellEffectInfo, MAX_SPELL_EFFECTS> const& effects)
{
    for (SpellEffectInfo const& effect : effects)
        if (ownedWave && effect.ApplyAuraName == SPELL_AURA_CHANNEL_DEATH_ITEM)
            return false;
    return true;
}

Creature* Owned(TownDefenseState const& state, ObjectGuid guid)
{
    Map* const map = sMapMgr->FindBaseNonInstanceMap(state.rally.mapId);
    Creature* const creature = map ? map->GetCreature(guid) : nullptr;
    // Never remove a persistent world spawn, even if the state became malformed.
    return creature && creature->IsSummon() && creature->GetSpawnId() == 0 ? creature : nullptr;
}

void RemoveWave(TownDefenseState const& state)
{
    for (ObjectGuid guid : state.wave)
        if (Creature* const creature = Owned(state, guid))
        {
            creature->CombatStop(true);
            creature->InterruptNonMeleeSpells(false);
            creature->DespawnOrUnsummon();
        }
}
}

bool TownDefenseWave::CanPrepare(Spell* spell)
{
    if (!spell || !spell->GetSpellInfo())
        return true;
    Unit* const target = spell->m_targets.GetUnitTarget();
    Creature* const creature = target ? target->ToCreature() : nullptr;
    bool const ownedWave = creature && creature->IsSummon() && creature->IsLootRewardDisabled() &&
        creature->CustomData.Get<DataMap::Base>("coa.activity.rewardless_wave");
    return DeathItemsAllowed(ownedWave, spell->GetSpellInfo()->Effects);
}

bool TownDefenseWave::SelfTest()
{
    std::array<SpellEffectInfo, MAX_SPELL_EFFECTS> effects;
    if (!DeathItemsAllowed(true, effects))
        return false;
    effects[0].ApplyAuraName = SPELL_AURA_PERIODIC_DAMAGE;
    if (!DeathItemsAllowed(true, effects))
        return false;
    for (SpellEffectInfo& effect : effects)
    {
        auto const original = effect.ApplyAuraName;
        effect.ApplyAuraName = SPELL_AURA_CHANNEL_DEATH_ITEM;
        if (DeathItemsAllowed(true, effects) || !DeathItemsAllowed(false, effects))
            return false;
        effect.ApplyAuraName = original;
    }
    return true;
}

CampaignSettings TownDefenseKind::Settings(ActivityConfig const& config) const
{
    CampaignSettings s;
    s.enabled = config.townDefenseEnable;
    s.minimum = config.townDefenseMinBots;
    s.maximum = config.townDefenseMaxBots;
    s.recruitSeconds = config.townDefenseRecruitSeconds;
    s.minutes = config.townDefenseMinutes;
    s.raid = s.maximum > 5;
    s.anchor = true;
    return s;
}

std::vector<ActivityCampaignTarget> const& TownDefenseKind::Configured(ActivityConfig const& config) const
{
    return config.townDefenseTargets;
}

bool TownDefenseKind::Validate(ActivityServices& services, ActivityStartRequest const& request,
                               ActivityCampaignTarget const& target, CampaignState& state, std::string& why) const
{
    if (!CampaignWorld::ResolveTarget(target, state.spawn, why))
        return false;
    CreatureTemplate const* const wave = target.waveEntry ? sObjectMgr->GetCreatureTemplate(target.waveEntry) : nullptr;
    if (!wave || wave->maxlevel > 60 || wave->minlevel < 1 || wave->npcflag || wave->ScriptID || !wave->AIName.empty() ||
        wave->rank == CREATURE_ELITE_WORLDBOSS || target.rally.mapId != state.spawn.point.mapId)
    {
        why = "waves require an explicit level-1..60 scriptless combat template, no NPC services, on the hub map";
        return false;
    }
    Creature* const anchor = ActivityGuards::FindSpawn(target.spawnId);
    if (!anchor || !anchor->IsAlive())
    {
        why = "town anchor must be loaded and alive";
        return false;
    }
    if (anchor->GetCreatureTemplate()->ScriptID || !anchor->GetCreatureTemplate()->AIName.empty())
    {
        why = "town anchor must be scriptless before owned-wave reward markers are installed";
        return false;
    }
    state.settings.lowest = uint8(std::max(1u, wave->maxlevel > 3 ? uint32(wave->maxlevel) - 3 : 1u));
    state.settings.highest = uint8(std::min(60u, uint32(wave->maxlevel) + 3));
    if (CampaignRules::NeedsHistory(request.gm, ActivityDirector::Instance().CleanRuns(ActivityKindId::TownDefense),
                                    services.config.townDefenseCleanWavesGate))
    {
        why = "ambient defense requires verified reward-free cleanup of the configured number of town waves";
        return false;
    }
    return true;
}

bool TownDefenseKind::Prepare(ActivityServices& services, Activity& activity, CampaignState& state)
{
    auto& town = static_cast<TownDefenseState&>(state);
    Creature* const anchor = ActivityGuards::FindSpawn(state.spawn.spawnId);
    Player* const leader = activity.Leader() ? ActivityWorld::FindMember(*activity.Leader()) : nullptr;
    if (!anchor || !leader || leader->GetMap() != anchor->GetMap() || !leader->IsFriendlyTo(anchor))
    {
        activity.Finish(ActivityOutcome::Stopped, "town anchor is not friendly to the defending group");
        return false;
    }
    town.wantedWaves = std::clamp(services.config.townDefenseWaves, 1u, 20u);
    town.waveSize = std::clamp(services.config.townDefenseWaveSize, 1u, 10u);
    return true;
}

void TownDefenseKind::Encounter(ActivityServices& services, Activity& activity, CampaignState& state)
{
    auto& town = static_cast<TownDefenseState&>(state);
    Creature* const anchor = ActivityGuards::FindSpawn(state.spawn.spawnId);
    Player* const leader = activity.Leader() ? ActivityWorld::FindMember(*activity.Leader()) : nullptr;
    if (!anchor || !anchor->IsAlive() || !leader || leader->GetMap() != anchor->GetMap())
    {
        activity.Finish(ActivityOutcome::Stopped, "town anchor or leader is unavailable");
        return;
    }
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
    activity.SetTravel(ActivityTravel::None);
    activity.SetCenter(town.rally, 30.0f);
    if (town.cleaning)
    {
        bool remains = false;
        for (ObjectGuid guid : town.wave)
            remains = remains || Owned(town, guid) != nullptr;
        if (remains)
        {
            if (services.nowMs >= town.cleanupDeadlineMs)
                activity.Finish(ActivityOutcome::Stopped, "owned wave cleanup did not finish within five seconds");
            return;
        }
        ++town.waves;
        ActivityDirector::Instance().RecordCleanWave();
        town.wave.clear();
        town.verifiedDead.clear();
        town.cleaning = false;
        town.nextWaveMs = services.nowMs + uint64(services.config.townDefenseWaveGapSeconds) * 1000;
        if (town.waves >= town.wantedWaves)
            activity.Finish(ActivityOutcome::Success, "all owned waves died without rewards and were removed");
        return;
    }
    if (town.wave.empty())
    {
        if (services.nowMs < town.nextWaveMs)
            return;
        for (uint32 i = 0; i < town.waveSize; ++i)
        {
            float const angle = float(i) * 6.2831853f / float(town.waveSize);
            ActivityPoint const point = ActivityGuards::GroundToward(town.rally,
                town.rally.x + 100.0f * std::cos(angle), town.rally.y + 100.0f * std::sin(angle), 35.0f);
            if (!point.set)
            {
                activity.Finish(ActivityOutcome::Stopped, "no dry ground for owned town wave");
                return;
            }
            TempSummon* const summon = anchor->GetMap()->SummonCreature(town.target.waveEntry,
                Position(point.x, point.y, point.z, angle), nullptr, 120000, anchor);
            if (!summon)
            {
                activity.Finish(ActivityOutcome::Stopped, "town wave summon failed");
                return;
            }
            town.wave.push_back(summon->GetGUID());
            summon->SetTempSummonType(TEMPSUMMON_TIMED_DESPAWN);
            // Unit::Kill gates both loot generation and RewardPlayerAndGroupAtKill on this runtime flag.
            summon->SetLootRewardDisabled(true);
            // A narrow optional CoA challenge dependency patch recognizes only these owned reward-free waves.
            summon->CustomData.Set("coa.activity.rewardless_wave", new DataMap::Base());
            summon->SetReputationRewardDisabled(true);
            summon->SetLootMode(0);
            summon->SetNoCallAssistance(true);
            summon->SetNoSearchAssistance(true);
            if (!leader->IsValidAttackTarget(summon) || !summon->IsValidAttackTarget(leader))
            {
                activity.Finish(ActivityOutcome::Stopped, "configured wave is not actually hostile to the PvE defenders");
                return;
            }
            summon->AI()->AttackStart(leader);
        }
        town.waveDeadlineMs = services.nowMs + 115000;
    }
    Creature* objective = nullptr;
    for (ObjectGuid guid : town.wave)
    {
        Creature* const creature = Owned(town, guid);
        if (!creature)
        {
            activity.Finish(ActivityOutcome::Stopped, "owned wave disappeared before its reward-free death was verified");
            return;
        }
        if (!creature->IsAlive())
        {
            if (!creature->IsLootRewardDisabled() || !creature->loot.empty() ||
                !creature->loot.quest_items.empty() || creature->loot.unlootedCount)
            {
                activity.Finish(ActivityOutcome::Stopped, Acore::StringFormat(
                    "owned wave reward suppression failed: disabled {} items {} quest-items {} gold {} remaining {} "
                    "recipient {} group {}", creature->IsLootRewardDisabled(), creature->loot.items.size(),
                    creature->loot.quest_items.size(), creature->loot.gold, creature->loot.unlootedCount,
                    creature->GetLootRecipientGUID().ToString(), creature->GetLootRecipientGroupGUID()));
                return;
            }
            // Spell::DoAllEffectOnTarget invokes AtTargetAttacked after lethal damage. That native helper can
            // reapply a tap to a corpse after Unit::Kill cleared it; it neither creates loot nor runs kill rewards.
            // Remove only that stale tag after checking the reward flag and every generated loot collection.
            if (creature->GetLootRecipientGUID() || creature->GetLootRecipientGroupGUID())
            {
                LOG_INFO("playerbots.activity", "activity #{} town wave corpse {}: disabled {}, items {}, "
                    "quest-items {}, gold {}, remaining {}; cleared post-death recipient {} group {}",
                    activity.id, guid.ToString(), creature->IsLootRewardDisabled(), creature->loot.items.size(),
                    creature->loot.quest_items.size(), creature->loot.gold, creature->loot.unlootedCount,
                    creature->GetLootRecipientGUID().ToString(), creature->GetLootRecipientGroupGUID());
                creature->SetLootRecipient(nullptr);
                ++town.clearedCorpseTags;
            }
            town.verifiedDead.insert(guid);
        }
        else if (creature->IsInEvadeMode() || creature->GetExactDist(town.rally.x, town.rally.y, town.rally.z) > 80.0f ||
                 services.nowMs >= town.waveDeadlineMs)
        {
            activity.Finish(ActivityOutcome::Stopped, "owned wave evaded, escaped, or exceeded its fight deadline");
            return;
        }
        else if (!objective)
            objective = creature;
    }
    activity.SetObjective(objective ? objective->GetGUID() : ObjectGuid::Empty);
    if (town.verifiedDead.size() == town.wave.size())
    {
        RemoveWave(town);
        town.cleaning = true;
        town.cleanupDeadlineMs = services.nowMs + 5000;
    }
}

void TownDefenseKind::Cleanup(ActivityServices&, Activity&, CampaignState& state)
{
    RemoveWave(static_cast<TownDefenseState const&>(state));
}

std::string TownDefenseKind::Describe(Activity const& activity, uint64 nowMs) const
{
    auto const* const state = dynamic_cast<TownDefenseState const*>(activity.state.get());
    return state ? Acore::StringFormat("{}; waves {}/{}, alive {}, verified dead {}, cleanup {}, corpse tags cleared {}",
        CampaignKind::Describe(activity, nowMs), state->waves, state->wantedWaves,
        state->wave.size() - std::min(state->wave.size(), state->verifiedDead.size()), state->verifiedDead.size(),
        state->cleaning ? "pending" : "no", state->clearedCorpseTags) : "town defense state unavailable";
}

std::unique_ptr<ActivityKind> CreateTownDefenseKind() { return std::make_unique<TownDefenseKind>(); }
