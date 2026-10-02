/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#include "LeaderScaler.h"
#include "CoaThread.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DynamicObject.h"
#include "LocalLevelScaling.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "ActivityRegistry.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "UpdateFields.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>

namespace
{
constexpr std::size_t MAX_GUARDS = 40;
constexpr std::size_t MAX_NOTIFICATIONS = 256;
constexpr char const* LOGGER = "playerbots.activity";
constexpr uint32 AURA_IMMUNITY_TAG = UINT32_MAX - 1;
constexpr std::array<uint32, 8> AURA_EFFECTS{SPELL_EFFECT_APPLY_AURA, SPELL_EFFECT_PERSISTENT_AREA_AURA,
    SPELL_EFFECT_APPLY_AREA_AURA_PARTY, SPELL_EFFECT_APPLY_AREA_AURA_RAID, SPELL_EFFECT_APPLY_AREA_AURA_PET,
    SPELL_EFFECT_APPLY_AREA_AURA_FRIEND, SPELL_EFFECT_APPLY_AREA_AURA_ENEMY, SPELL_EFFECT_APPLY_AREA_AURA_OWNER};
using ImmunitySnapshot = std::array<std::vector<std::pair<uint32, uint32>>, MAX_SPELL_IMMUNITY>;

struct StatSnapshot
{
    std::vector<std::pair<uint16, uint32>> fields;
    std::array<std::array<float, MODIFIER_TYPE_FLAT_END>, UNIT_MOD_END> flat{};
    std::array<std::array<float, MODIFIER_TYPE_PCT_END>, UNIT_MOD_END> pct{};
    std::array<std::array<std::array<float, MAX_ITEM_PROTO_DAMAGES>, MAX_WEAPON_DAMAGE_RANGE>, MAX_ATTACK> weapon{};
    std::array<float, MAX_STATS> createStats{};
    std::array<float, MAX_MOVE_TYPE> speeds{};
    std::array<float, MAX_ATTACK> attackSpeed{};
    std::array<float, 3> hit{};
    int32 spellCrit = 0;
    uint16 lootMode = 0;
    bool lootDisabled = false;
    bool reputationDisabled = false;
    bool modifyStats = true;
    bool offhand = false;
    uint32 health = 0;
    ImmunitySnapshot immunities;
};

struct LeaseRow
{
    uint32 activityId = 0;
    uint64 generation = 0;
    ActivityKindId kind = ActivityKindId::LeaderEncounter;
    ObjectGuid guid;
    uint32 mapId = 0;
    uint32 instanceId = 0;
    uint32 spawnId = 0;
    uint32 entry = 0;
    uint32 scaledHealth = 0;
    uint32 damagePercent = 100;
    bool leader = false;
    bool restoredDead = false;
    bool restoredRemoved = false;
    bool retiring = false;
    StatSnapshot original;
    std::vector<LeaderScaleParticipant> participants;
};

struct Published
{
    std::unordered_map<ObjectGuid, LeaseRow> creatures;
};

enum class NoticeKind : uint8 { Death, Remove, Intrusion, ForeignDeath };
struct Notice
{
    uint32 activityId = 0;
    uint64 generation = 0;
    ObjectGuid guid;
    NoticeKind kind = NoticeKind::Remove;
};

struct Encounter
{
    uint64 generation = 0;
    ObjectGuid leader;
    std::vector<LeaseRow> rows;
    bool fought = false;
    ActivityPoint origin;
    float projectorRadius = 0;
};

struct State
{
    std::map<uint32, Encounter> encounters;  // world-thread writer only
    std::vector<LeaseRow> lastRestored;  // at most one completed encounter, plain identifiers + scalar data
    uint64 nextGeneration = 1;
    std::mutex gate;
    std::shared_ptr<Published const> published = std::make_shared<Published>();
    std::deque<Notice> notices;
    std::map<std::pair<ObjectGuid, uint64>, NoticeKind> released;  // map-owned restoration completed; never touch a respawn
    bool overflow = false;
    std::atomic<bool> any{false};
};

State& Storage()
{
    static State state;
    return state;
}

std::shared_ptr<Published const> Current()
{
    State& state = Storage();
    std::lock_guard<std::mutex> lock(state.gate);
    return state.published;
}

bool Released(LeaseRow const& row)
{
    State& state = Storage();
    std::lock_guard<std::mutex> lock(state.gate);
    return state.released.count({row.guid, row.generation}) != 0;
}

void Publish()
{
    State& state = Storage();
    auto next = std::make_shared<Published>();
    for (auto const& [id, encounter] : state.encounters)
        for (LeaseRow const& row : encounter.rows)
            if (!Released(row))
                next->creatures.emplace(row.guid, row);
    {
        std::lock_guard<std::mutex> lock(state.gate);
        state.published = std::move(next);
    }
    state.any.store(!state.encounters.empty(), std::memory_order_release);
}

void Notify(LeaseRow const& row, NoticeKind kind)
{
    State& state = Storage();
    std::lock_guard<std::mutex> lock(state.gate);
    auto const duplicate = std::find_if(state.notices.begin(), state.notices.end(), [&](Notice const& item)
    {
        return item.generation == row.generation && item.guid == row.guid && item.kind == kind;
    });
    if (duplicate != state.notices.end())
        return;
    if (state.notices.size() >= MAX_NOTIFICATIONS)
        state.overflow = true;
    else
        state.notices.push_back({row.activityId, row.generation, row.guid, kind});
}

bool Matches(Creature* creature, LeaseRow const& row)
{
    return creature && creature->GetMap() && creature->GetGUID() == row.guid && creature->GetEntry() == row.entry &&
        creature->GetSpawnId() == row.spawnId && creature->GetMapId() == row.mapId &&
        creature->GetInstanceId() == row.instanceId;
}

Creature* Resolve(LeaseRow const& row)
{
    Map* const map = sMapMgr->FindMap(row.mapId, row.instanceId);
    Creature* const creature = map ? map->GetCreature(row.guid) : nullptr;
    return Matches(creature, row) ? creature : nullptr;
}

bool Finite(float value) { return std::isfinite(value) && std::fabs(value) <= 1000000000.0f; }

uint32 FloatBits(float value)
{
    uint32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

ImmunitySnapshot Immunities(Unit const* unit)
{
    ImmunitySnapshot result;
    for (uint8 op = 0; op < MAX_SPELL_IMMUNITY; ++op)
    {
        result[op].assign(unit->m_spellImmune[op].begin(), unit->m_spellImmune[op].end());
        std::sort(result[op].begin(), result[op].end());
    }
    return result;
}

void AuraImmunity(Creature* creature, bool apply)
{
    for (uint32 state = 1; state < TOTAL_AURAS; ++state)
        creature->ApplySpellImmune(AURA_IMMUNITY_TAG, IMMUNITY_STATE, state, apply);
    for (uint32 effect : AURA_EFFECTS)
        creature->ApplySpellImmune(AURA_IMMUNITY_TAG, IMMUNITY_EFFECT, effect, apply);
}

bool ReachesUnownedNpc(uint32 effect)
{
    return effect == SPELL_EFFECT_APPLY_AREA_AURA_PARTY || effect == SPELL_EFFECT_APPLY_AREA_AURA_RAID ||
        effect == SPELL_EFFECT_APPLY_AREA_AURA_FRIEND || effect == SPELL_EFFECT_APPLY_AREA_AURA_ENEMY ||
        effect == SPELL_EFFECT_PERSISTENT_AREA_AURA;
}

bool ProjectsToNpc(Aura const* aura)
{
    SpellInfo const* const spell = aura->GetSpellInfo();
    return aura->IsArea() && std::any_of(spell->Effects.begin(), spell->Effects.end(), [](SpellEffectInfo const& effect)
    { return ReachesUnownedNpc(effect.Effect); });
}

bool HasUnitProjector(Unit* unit)
{
    return std::any_of(unit->GetOwnedAuras().begin(), unit->GetOwnedAuras().end(), [](auto const& item)
    { return ProjectsToNpc(item.second); });
}

bool HasGroundProjector(Unit* unit)
{
    // The native owned dynamic-object list is protected. Spell IDs plus the public lookup provide
    // a bounded read without loading grids, retaining a DynamicObject pointer or deleting an aura.
    static std::vector<uint32> ids;
    static uint32 storeSize = 0;
    if (storeSize != sSpellMgr->GetSpellInfoStoreSize())
    {
        storeSize = sSpellMgr->GetSpellInfoStoreSize();
        ids.clear();
        for (uint32 id = 0; id < storeSize; ++id)
            if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(id); spell &&
                std::any_of(spell->Effects.begin(), spell->Effects.end(), [](SpellEffectInfo const& effect)
                { return effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA; }))
                ids.push_back(id);
    }
    for (uint32 id : ids)
        if (DynamicObject* object = unit->GetDynObject(id); object && object->IsInWorld())
            return true;
    return false;
}

bool Projectors(Unit* unit, float& radius, std::string& why)
{
    if (HasGroundProjector(unit))
    {
        why = "a caster-owned ground area must expire before admission or final rollback";
        return false;
    }
    for (auto const& [id, aura] : unit->GetOwnedAuras())
    {
        // Owner/pet/summon-only scaling auras cannot target an admitted unowned static NPC.
        SpellInfo const* const spell = aura->GetSpellInfo();
        if (!ProjectsToNpc(aura))
            continue;
        if (spell->HasAttribute(SPELL_ATTR0_NO_IMMUNITIES) || spell->HasAttribute(SPELL_ATTR4_OWNER_POWER_SCALING))
        {
            why = "active projecting aura " + std::to_string(id) + " bypasses native aura immunities";
            return false;
        }
        for (SpellEffectInfo const& effect : spell->Effects)
            if (effect.IsAreaAuraEffect() || effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA)
            {
                float const range = effect.CalcRadius(aura->GetCaster());
                if (!std::isfinite(range) || range < 0 || range > 5000)
                { why = "active projecting aura has an unsupported radius"; return false; }
                radius = std::max(radius, range);
            }
    }
    return true;
}

bool Capture(Creature* creature, StatSnapshot& out, std::string& why)
{
    if (!creature || !creature->IsInWorld() || !creature->GetMap() || creature->GetMap()->Instanceable() ||
        creature->IsDuringRemoveFromWorld() || !creature->IsAlive() || creature->IsInCombat() ||
        creature->IsInEvadeMode() || creature->IsEngaged() || !creature->GetMaxHealth() ||
        !creature->GetLevel() || !creature->IsFullHealth() || !creature->GetSpawnId() ||
        creature->IsSummon() || creature->GetCharmerOrOwnerGUID() || creature->GetVehicleKit() ||
        creature->GetTransport() || creature->hasLootRecipient() || !creature->CanModifyStats())
    {
        why = "a leased spawn must be loaded, healthy, unowned, untagged, static and out of combat in the overworld";
        return false;
    }
    // Aura applications have their own evolving state. Reconstructing them from scalar stats is not reversible.
    if (!creature->GetAppliedAuras().empty())
    {
        why = "a leased spawn has active auras that cannot be restored by a scalar snapshot";
        return false;
    }
    std::size_t immunityCount = 0;
    for (auto const& list : creature->m_spellImmune)
    {
        immunityCount += list.size();
        for (auto const& [type, spell] : list)
            if (spell == AURA_IMMUNITY_TAG)
            { why = "the runtime aura-immunity tag is already owned"; return false; }
    }
    if (immunityCount > 4096)
    { why = "the native immunity metadata exceeds the bounded snapshot"; return false; }
    out.immunities = Immunities(creature);
    CreatureTemplate const* const prototype = creature->GetCreatureTemplate();
    if (!prototype || prototype->SkinLootId || creature->GetFormation())
    {
        why = "skinnable or formation-controlled spawn has side effects outside an explicit scalar lease";
        return false;
    }
    std::string const& script = sObjectMgr->GetScriptName(prototype->ScriptID);
    if ((!script.empty() && script != "npc_king_varian_wrynn" && script != "npc_king_magni_bronzebeard" &&
         script != "npc_cairne_bloodhoof" && script != "npc_thrall_warchief") ||
        (!prototype->AIName.empty() && prototype->AIName != "AggressorAI" && prototype->AIName != "GuardAI" &&
         prototype->AIName != "CombatAI"))
    {
        why = "unreviewed scripted/SmartAI spawn can change state outside a reversible scalar lease";
        return false;
    }
    auto field = [&](uint16 index) { out.fields.emplace_back(index, creature->GetUInt32Value(index)); };
    auto range = [&](uint16 index, uint16 count) { for (uint16 i = 0; i < count; ++i) field(index + i); };
    field(UNIT_FIELD_LEVEL);
    field(UNIT_FIELD_HEALTH);
    field(UNIT_FIELD_MAXHEALTH);
    field(UNIT_FIELD_BASE_HEALTH);
    field(UNIT_FIELD_BASE_MANA);
    field(UNIT_FIELD_MAXHEALTHMODIFIER);
    range(UNIT_FIELD_POWER1, MAX_POWERS);
    range(UNIT_FIELD_MAXPOWER1, MAX_POWERS);
    range(UNIT_FIELD_STAT0, MAX_STATS);
    range(UNIT_FIELD_POSSTAT0, MAX_STATS);
    range(UNIT_FIELD_NEGSTAT0, MAX_STATS);
    range(UNIT_FIELD_RESISTANCES, MAX_SPELL_SCHOOL);
    range(UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE, MAX_SPELL_SCHOOL);
    range(UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE, MAX_SPELL_SCHOOL);
    range(UNIT_FIELD_BASEATTACKTIME, 2);
    field(UNIT_FIELD_RANGEDATTACKTIME);
    range(UNIT_FIELD_MINDAMAGE, 4);
    range(UNIT_FIELD_ATTACK_POWER, 6);
    range(UNIT_FIELD_MINRANGEDDAMAGE, 2);
    for (uint16 mod = 0; mod < UNIT_MOD_END; ++mod)
    {
        for (uint8 type = 0; type < MODIFIER_TYPE_FLAT_END; ++type)
        {
            float const value = creature->GetFlatModifierValue(UnitMods(mod), UnitModifierFlatType(type));
            if (!Finite(value)) { why = "nonfinite stat modifier"; return false; }
            out.flat[mod][type] = value;
        }
        for (uint8 type = 0; type < MODIFIER_TYPE_PCT_END; ++type)
        {
            float const value = creature->GetPctModifierValue(UnitMods(mod), UnitModifierPctType(type));
            if (!Finite(value)) { why = "nonfinite stat multiplier"; return false; }
            out.pct[mod][type] = value;
        }
    }
    out.offhand = creature->HasOffhandWeaponForAttack();
    for (uint8 attack = 0; attack < MAX_ATTACK; ++attack)
        for (uint8 rangeId = 0; rangeId < MAX_WEAPON_DAMAGE_RANGE; ++rangeId)
            for (uint8 index = 0; index < MAX_ITEM_PROTO_DAMAGES; ++index)
            {
                float const value = creature->GetWeaponDamageRange(WeaponAttackType(attack),
                                                                  WeaponDamageRange(rangeId), index);
                if (!Finite(value) || value < 0) { why = "invalid weapon damage"; return false; }
                out.weapon[attack][rangeId][index] = value;
            }
    for (uint8 stat = 0; stat < MAX_STATS; ++stat)
    {
        out.createStats[stat] = creature->GetCreateStat(Stats(stat));
        if (!Finite(out.createStats[stat])) { why = "invalid create stat"; return false; }
    }
    for (uint8 move = 0; move < MAX_MOVE_TYPE; ++move)
    {
        out.speeds[move] = creature->GetSpeedRate(UnitMoveType(move));
        if (!Finite(out.speeds[move]) || out.speeds[move] < 0) { why = "invalid movement speed"; return false; }
    }
    for (uint8 attack = 0; attack < MAX_ATTACK; ++attack)
    {
        out.attackSpeed[attack] = creature->m_modAttackSpeedPct[attack];
        if (!Finite(out.attackSpeed[attack]) || out.attackSpeed[attack] <= 0)
        { why = "invalid attack speed"; return false; }
    }
    out.hit = {creature->m_modMeleeHitChance, creature->m_modRangedHitChance, creature->m_modSpellHitChance};
    if (!std::all_of(out.hit.begin(), out.hit.end(), Finite)) { why = "invalid hit modifier"; return false; }
    out.spellCrit = creature->m_baseSpellCritChance;
    out.modifyStats = creature->CanModifyStats();
    out.lootMode = creature->GetLootMode();
    out.lootDisabled = creature->IsLootRewardDisabled();
    out.reputationDisabled = creature->IsReputationRewardDisabled();
    out.health = creature->GetHealth();
    return true;
}

void RestoreCreature(Creature* creature, StatSnapshot const& original, bool restoreRewards)
{
    bool const alive = creature->IsAlive();
    // Capture admitted no original aura applications; discard any combat-added applications before raw rollback.
    creature->RemoveAllAuras();
    creature->SetCanModifyStats(false);
    for (uint16 mod = 0; mod < UNIT_MOD_END; ++mod)
    {
        for (uint8 type = 0; type < MODIFIER_TYPE_FLAT_END; ++type)
            creature->SetStatFlatModifier(UnitMods(mod), UnitModifierFlatType(type), original.flat[mod][type]);
        for (uint8 type = 0; type < MODIFIER_TYPE_PCT_END; ++type)
            creature->SetStatPctModifier(UnitMods(mod), UnitModifierPctType(type), original.pct[mod][type]);
    }
    for (uint8 attack = 0; attack < MAX_ATTACK; ++attack)
    {
        // The public getter hides offhand storage without an equipped offhand; that storage is never changed.
        if (attack == OFF_ATTACK && !original.offhand)
            continue;
        for (uint8 rangeId = 0; rangeId < MAX_WEAPON_DAMAGE_RANGE; ++rangeId)
            for (uint8 index = 0; index < MAX_ITEM_PROTO_DAMAGES; ++index)
                creature->SetBaseWeaponDamage(WeaponAttackType(attack), WeaponDamageRange(rangeId),
                                             original.weapon[attack][rangeId][index], index);
    }
    for (uint8 stat = 0; stat < MAX_STATS; ++stat)
        creature->SetCreateStat(Stats(stat), original.createStats[stat]);
    for (uint8 attack = 0; attack < MAX_ATTACK; ++attack)
        creature->m_modAttackSpeedPct[attack] = original.attackSpeed[attack];
    creature->m_modMeleeHitChance = original.hit[0];
    creature->m_modRangedHitChance = original.hit[1];
    creature->m_modSpellHitChance = original.hit[2];
    creature->m_baseSpellCritChance = original.spellCrit;
    // Restore exact update-field bit patterns after modifier setters, not an approximate recomputation.
    for (auto const& [index, value] : original.fields)
        if (index != UNIT_FIELD_HEALTH)
            creature->SetUInt32Value(index, !alive && index >= UNIT_FIELD_POWER1 &&
                                    index < UNIT_FIELD_POWER1 + MAX_POWERS ? 0 : value);
    creature->SetHealth(alive ? original.health : 0);
    for (uint8 move = 0; move < MAX_MOVE_TYPE; ++move)
        creature->SetSpeed(UnitMoveType(move), original.speeds[move]);
    creature->SetCanModifyStats(original.modifyStats);
    if (restoreRewards)
    {
        creature->SetLootMode(original.lootMode);
        creature->SetLootRewardDisabled(original.lootDisabled);
        creature->SetReputationRewardDisabled(original.reputationDisabled);
    }
    // Removing our exact tag/type pairs leaves even duplicate native immunity records untouched.
    AuraImmunity(creature, false);
    // Read back before yielding to map updates. A later native aura/speed callback must remain
    // distinguishable from a rollback that never reinstated the captured rate in the first place.
    uint32 speedMismatches = 0;
    for (uint8 move = 0; move < MAX_MOVE_TYPE; ++move)
    {
        float const actual = creature->GetSpeedRate(UnitMoveType(move));
        if (FloatBits(actual) != FloatBits(original.speeds[move]))
        {
            ++speedMismatches;
            LOG_ERROR(LOGGER, "LeaderScaler immediate restore speed mismatch spawn {} entry {} map {} instance {} "
                      "mode {} expected {} (bits {}) actual {} (bits {})",
                      creature->GetSpawnId(), creature->GetEntry(), creature->GetMapId(), creature->GetInstanceId(),
                      uint32(move), original.speeds[move], FloatBits(original.speeds[move]), actual, FloatBits(actual));
        }
    }
    LOG_INFO(LOGGER, "LeaderScaler immediate restore speed readback spawn {} entry {} map {} instance {}: "
             "{} mismatches; health {}/{}; applied auras {}; native immunity metadata restored {}",
             creature->GetSpawnId(), creature->GetEntry(), creature->GetMapId(), creature->GetInstanceId(),
             speedMismatches, creature->GetHealth(), creature->GetMaxHealth(), creature->GetAppliedAuras().size(),
             Immunities(creature) == original.immunities);
}

void Apply(Creature* creature, LeaseRow const& row, LeaderScaleParameters const& parameters)
{
    // Aura::UpdateTargetMap and direct AddAura also consult native effect/state immunities, unlike spell hooks.
    AuraImmunity(creature, true);
    StatSnapshot const& original = row.original;
    float const damage = float(row.damagePercent) / 100.0f;
    float const armor = float(parameters.armorPercent) / 100.0f;
    creature->SetCanModifyStats(false);
    creature->SetLevel(std::min(parameters.level, creature->GetLevel()));
    creature->SetCreateHealth(row.scaledHealth);
    for (uint8 type = 0; type < MODIFIER_TYPE_FLAT_END; ++type)
        creature->SetStatFlatModifier(UNIT_MOD_HEALTH, UnitModifierFlatType(type), type == BASE_VALUE ? row.scaledHealth : 0);
    for (uint8 type = 0; type < MODIFIER_TYPE_PCT_END; ++type)
        creature->SetStatPctModifier(UNIT_MOD_HEALTH, UnitModifierPctType(type), 1.0f);
    for (UnitMods const mod : {UNIT_MOD_ARMOR, UNIT_MOD_ATTACK_POWER, UNIT_MOD_ATTACK_POWER_RANGED})
        for (uint8 type = 0; type < MODIFIER_TYPE_FLAT_END; ++type)
            creature->SetStatFlatModifier(mod, UnitModifierFlatType(type), original.flat[mod][type] *
                                         (mod == UNIT_MOD_ARMOR ? armor : damage));
    for (UnitMods const mod : {UNIT_MOD_DAMAGE_MAINHAND, UNIT_MOD_DAMAGE_OFFHAND, UNIT_MOD_DAMAGE_RANGED})
        for (uint8 type = 0; type < MODIFIER_TYPE_FLAT_END; ++type)
            creature->SetStatFlatModifier(mod, UnitModifierFlatType(type), original.flat[mod][type] * damage);
    for (uint8 attack = 0; attack < MAX_ATTACK; ++attack)
    {
        if (attack == OFF_ATTACK && !original.offhand)
            continue;
        for (uint8 rangeId = 0; rangeId < MAX_WEAPON_DAMAGE_RANGE; ++rangeId)
            for (uint8 index = 0; index < MAX_ITEM_PROTO_DAMAGES; ++index)
                creature->SetBaseWeaponDamage(WeaponAttackType(attack), WeaponDamageRange(rangeId),
                                             original.weapon[attack][rangeId][index] * damage, index);
    }
    creature->SetCanModifyStats(true);
    creature->UpdateArmor();
    creature->UpdateAttackPowerAndDamage();
    creature->UpdateAttackPowerAndDamage(true);
    creature->SetMaxHealth(row.scaledHealth);
    creature->SetHealth(row.scaledHealth);
    creature->SetLootMode(0);
    creature->SetLootRewardDisabled(true);
    creature->SetReputationRewardDisabled(true);
}

LeaseRow const* Row(Unit* unit, Published const& snapshot)
{
    Creature* const creature = unit ? unit->ToCreature() : nullptr;
    auto const found = creature ? snapshot.creatures.find(creature->GetGUID()) : snapshot.creatures.end();
    return found != snapshot.creatures.end() && Matches(creature, found->second) && !Released(found->second)
        ? &found->second : nullptr;
}

bool Participant(Unit* unit, LeaseRow const& row)
{
    Player* const player = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!player || !unit->GetMap() || player->GetMap() != unit->GetMap() || player->GetMapId() != row.mapId ||
        player->GetInstanceId() != row.instanceId)
        return false;
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(player);
    auto const registry = ai ? ActivityRegistry::Find(player->GetGUID().GetCounter()) : std::nullopt;
    return ai && registry && registry->activityId == row.activityId && registry->epoch == ai->GetCoaEpoch() &&
        registry->kind == row.kind &&
        std::any_of(row.participants.begin(), row.participants.end(), [&](auto const& member)
    {
        return member.guid == player->GetGUID() && member.epoch == ai->GetCoaEpoch();
    });
}

LeaseRow const* MemberLease(Unit* unit, Published const& snapshot)
{
    for (auto const& [guid, row] : snapshot.creatures)
        if (row.leader && Participant(unit, row))
            return &row;
    return nullptr;
}

bool SameLease(LeaseRow const* a, LeaseRow const* b)
{
    return a && b && a->activityId == b->activityId && a->generation == b->generation;
}

bool Permitted(LeaseRow const* from, LeaseRow const* to, LeaseRow const* actor, LeaseRow const* victim)
{
    return (!from || SameLease(from, to) || SameLease(from, victim)) &&
        (!to || SameLease(to, from) || SameLease(to, actor)) &&
        (!actor || SameLease(actor, to) || SameLease(actor, victim)) &&
        (!victim || SameLease(victim, from) || SameLease(victim, actor));
}

void ImmediateRestore(Creature* creature, NoticeKind kind, Unit* killer = nullptr)
{
    if (!LeaderScaler::HasLeases() || !creature)
        return;
    auto const snapshot = Current();
    LeaseRow const* const row = Row(creature, *snapshot);
    if (!row)
        return;
    if (kind == NoticeKind::Death && !Participant(killer, *row))
        kind = NoticeKind::ForeignDeath;
    {
        State& state = Storage();
        std::lock_guard<std::mutex> lock(state.gate);
        if (!state.released.emplace(std::make_pair(row->guid, row->generation), kind).second)
            return;
    }
    // OnUnitDeath runs after native kill rewards and scripted death callbacks. Restore reward flags too;
    // the corpse was already generated with rewards disabled, and a later respawn must be wholly authored.
    RestoreCreature(creature, row->original, true);
    Notify(*row, kind);
}
}  // namespace

uint32 LeaderScalerRules::ScaleDamage(uint32 damage, uint32 percent)
{
    return uint32(std::min<uint64>(UINT32_MAX, (uint64(damage) * std::min<uint32>(percent, 100)) / 100));
}

bool LeaderScalerRules::Valid(LeaderScaleParameters const& parameters)
{
    return parameters.level >= 1 && parameters.level <= 60 && parameters.health >= 1 &&
        parameters.health <= 1000000000 && parameters.guardHealth >= 1 && parameters.guardHealth <= 1000000000 &&
        parameters.damagePercent >= 1 && parameters.damagePercent <= 100 &&
        parameters.guardDamagePercent >= 1 && parameters.guardDamagePercent <= 100 &&
        parameters.armorPercent >= 1 && parameters.armorPercent <= 100;
}

bool LeaderScalerRules::SpellEffectAllowed(uint32 effect)
{
    // These effects change ordinary combat state. Scripts, world-object creation, rewards, transforms,
    // teleports and custom spell effects cannot be restored from a scalar snapshot.
    switch (effect)
    {
        case 0:
        case SPELL_EFFECT_SCHOOL_DAMAGE:
        case SPELL_EFFECT_APPLY_AURA:
        case SPELL_EFFECT_POWER_DRAIN:
        case SPELL_EFFECT_HEALTH_LEECH:
        case SPELL_EFFECT_HEAL:
        case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
        case SPELL_EFFECT_ENERGIZE:
        case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
        case SPELL_EFFECT_DISPEL:
        case SPELL_EFFECT_WEAPON_DAMAGE:
        case SPELL_EFFECT_POWER_BURN:
        case SPELL_EFFECT_THREAT:
        case SPELL_EFFECT_TRIGGER_SPELL:
        case SPELL_EFFECT_HEAL_MAX_HEALTH:
        case SPELL_EFFECT_INTERRUPT_CAST:
        case SPELL_EFFECT_PULL:
        case SPELL_EFFECT_ATTACK:
        case SPELL_EFFECT_ADD_COMBO_POINTS:
        case SPELL_EFFECT_THREAT_ALL:
        case SPELL_EFFECT_KNOCK_BACK:
        case SPELL_EFFECT_DISPEL_MECHANIC:
        case SPELL_EFFECT_ATTACK_ME:
        case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
        case SPELL_EFFECT_PULL_TOWARDS:
        case SPELL_EFFECT_MODIFY_THREAT_PERCENT:
        case SPELL_EFFECT_HEAL_PCT:
        case SPELL_EFFECT_ENERGIZE_PCT:
        case SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE:
        case SPELL_EFFECT_KNOCK_BACK_DEST:
        case SPELL_EFFECT_PULL_TOWARDS_DEST:
        case SPELL_EFFECT_TRIGGER_SPELL_2:
        case SPELL_EFFECT_REMOVE_AURA:
            return true;
        default:
            return false;
    }
}

bool LeaderScaler::HasLeases() { return Storage().any.load(std::memory_order_acquire); }

bool LeaderScaler::Begin(uint32 activityId, Creature* leader, std::vector<Creature*> const& guards,
                         std::vector<LeaderScaleParticipant> const& participants,
                         LeaderScaleParameters const& parameters, std::string& why, ActivityKindId kind)
{
    COA_ASSERT_WORLD_THREAD();
    State& state = Storage();
    if (!activityId || !leader || participants.empty() || participants.size() > 40 || guards.size() > MAX_GUARDS ||
        !LeaderScalerRules::Valid(parameters) || state.encounters.count(activityId) || !state.encounters.empty() ||
        (kind != ActivityKindId::LeaderEncounter && kind != ActivityKindId::CapitalRaid && kind != ActivityKindId::LieutenantRaid))
    { why = "invalid scale parameters, participant/guard count, or an existing leader lease"; return false; }
    std::set<ObjectGuid> people;
    float projectorRadius = 0;
    for (auto const& member : participants)
    {
        Player* const bot = ObjectAccessor::FindConnectedPlayer(member.guid);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!member.epoch || !people.insert(member.guid).second || !ai || ai->GetCoaEpoch() != member.epoch ||
            !bot->IsInWorld() || !bot->IsAlive() || bot->GetMap() != leader->GetMap() ||
            LocalLevelScaling::ScalingChoiceEnabled(bot))
        { why = "participant lifetime/map mismatch or per-viewer scaling would conflict with the encounter"; return false; }
        if (!Projectors(bot, projectorRadius, why))
            return false;
        for (Unit* controlled : bot->m_Controlled)
            if (controlled && controlled->GetMap() == leader->GetMap() && !Projectors(controlled, projectorRadius, why))
                return false;
    }
    Encounter encounter;
    encounter.generation = state.nextGeneration++;
    encounter.leader = leader->GetGUID();
    encounter.origin = {leader->GetMapId(), leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ(),
                        leader->GetZoneId(), true};
    encounter.projectorRadius = projectorRadius;
    std::vector<Creature*> creatures{leader};
    creatures.insert(creatures.end(), guards.begin(), guards.end());
    std::set<ObjectGuid> seen;
    for (Creature* creature : creatures)
    {
        LeaseRow row;
        if (!creature || creature->GetMap() != leader->GetMap() || !creature->InSamePhase(leader) ||
            !seen.insert(creature->GetGUID()).second || !Capture(creature, row.original, why))
        { if (why.empty()) why = "duplicate guard or guard on another map/phase"; return false; }
        row.activityId = activityId;
        row.kind = kind;
        row.generation = encounter.generation;
        row.guid = creature->GetGUID();
        row.mapId = creature->GetMapId();
        row.instanceId = creature->GetInstanceId();
        row.spawnId = creature->GetSpawnId();
        row.entry = creature->GetEntry();
        row.leader = creature == leader;
        row.scaledHealth = std::min(creature->GetMaxHealth(), row.leader ? parameters.health : parameters.guardHealth);
        row.damagePercent = row.leader ? parameters.damagePercent : parameters.guardDamagePercent;
        row.participants = participants;
        encounter.rows.push_back(std::move(row));
    }
    // All validation and captures finish before the first mutation; any application failure rolls back every row.
    state.encounters.emplace(activityId, std::move(encounter));
    Publish();
    try
    {
        Encounter const& installed = state.encounters.at(activityId);
        for (std::size_t i = 0; i < installed.rows.size(); ++i)
            Apply(creatures[i], installed.rows[i], parameters);
    }
    catch (...)
    {
        Restore(activityId);
        why = "exception while applying scale; all live identities restored";
        return false;
    }
    LOG_INFO(LOGGER, "activity #{} leader scale: {} static spawn instances leased, target level {}",
             activityId, creatures.size(), uint32(parameters.level));
    return true;
}

LeaderScaleResult LeaderScaler::Poll(uint32 activityId, std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    State& state = Storage();
    auto const found = state.encounters.find(activityId);
    if (found == state.encounters.end()) { why = "the leader lease was restored"; return LeaderScaleResult::Restored; }
    Encounter& encounter = found->second;
    bool overflow = false;
    std::vector<Notice> relevant;
    {
        std::lock_guard<std::mutex> lock(state.gate);
        overflow = state.overflow;
        for (auto it = state.notices.begin(); it != state.notices.end();)
            if (it->activityId == activityId)
            {
                if (it->generation == encounter.generation)
                    relevant.push_back(*it);
                it = state.notices.erase(it);
            }
            else ++it;
    }
    if (overflow) { why = "leader notification mailbox overflowed"; return LeaderScaleResult::Changed; }
    for (Notice const& notice : relevant)
    {
        if (notice.kind == NoticeKind::Intrusion)
        { why = "an unrelated unit attempted to interfere with the leased encounter"; return LeaderScaleResult::Changed; }
        if (notice.kind == NoticeKind::ForeignDeath)
        { why = "a leased spawn died without a current bot member's native kill"; return LeaderScaleResult::Changed; }
        if (notice.guid == encounter.leader)
        {
            why = notice.kind == NoticeKind::Death ? "the leased leader died" : "the leased leader was removed";
            return notice.kind == NoticeKind::Death ? LeaderScaleResult::Killed : LeaderScaleResult::Lost;
        }
    }
    Creature* leader = nullptr;
    for (LeaseRow const& row : encounter.rows)
    {
        if (Released(row) && !row.leader)
            continue;
        Creature* const creature = Resolve(row);
        if (!creature || !creature->IsInWorld())
        { why = "a leased spawn instance disappeared"; return LeaderScaleResult::Lost; }
        if (row.leader)
            leader = creature;
        if (!creature->IsAlive())
        {
            why = "a leased spawn died without the registered native kill notification";
            return LeaderScaleResult::Changed;
        }
        if (creature->IsInEvadeMode())
        { why = "a leased spawn entered evade"; return LeaderScaleResult::Evaded; }
        if (creature->GetMaxHealth() != row.scaledHealth || !creature->GetAppliedAuras().empty())
        {
            auto const& auras = creature->GetAppliedAuras();
            Aura const* const firstAura = auras.empty() ? nullptr : auras.begin()->second->GetBase();
            uint32 const auraId = firstAura ? firstAura->GetId() : 0;
            std::string const caster = firstAura ? firstAura->GetCasterGUID().ToString() : "none";
            why = "a script or aura changed leased spawn " + std::to_string(row.spawnId) +
                " entry " + std::to_string(row.entry) + " (max health " +
                std::to_string(creature->GetMaxHealth()) + "/" + std::to_string(row.scaledHealth) +
                "; applied auras " + std::to_string(auras.size()) + "; first spell " +
                std::to_string(auraId) + "; caster " + caster + ")";
            LOG_ERROR(LOGGER, "LeaderScaler changed activity {} generation {} spawn {} entry {} map {} instance {}: "
                      "health {} expected max {} actual max {}; applied auras {}; first spell {} caster {}",
                      activityId, row.generation, row.spawnId, row.entry, row.mapId, row.instanceId,
                      creature->GetHealth(), row.scaledHealth, creature->GetMaxHealth(), auras.size(), auraId, caster);
            return LeaderScaleResult::Changed;
        }
        encounter.fought = encounter.fought || creature->IsInCombat() || creature->GetHealth() < creature->GetMaxHealth();
    }
    if (encounter.fought && leader && !leader->IsInCombat() &&
        leader->GetExactDist2d(leader->GetHomePosition()) <= 5.0f && leader->IsFullHealth())
    { why = "the leader returned home after combat"; return LeaderScaleResult::Evaded; }
    why.clear();
    return LeaderScaleResult::Active;
}

float LeaderScaler::BeginWithdrawal(uint32 activityId)
{
    COA_ASSERT_WORLD_THREAD();
    auto const found = Storage().encounters.find(activityId);
    if (found == Storage().encounters.end())
        return 0;
    Encounter& encounter = found->second;
    for (LeaseRow& row : encounter.rows)
        row.retiring = true;
    Publish();  // reject native attacks/spells before evade callbacks run
    for (LeaseRow const& row : encounter.rows)
        if (!Released(row))
            if (Creature* creature = Resolve(row); creature && creature->IsAlive())
            {
                if (creature->IsAIEnabled && creature->AI())
                    creature->AI()->EnterEvadeMode();
                else
                    creature->CombatStop(true);
            }
    float spread = 0;
    for (LeaseRow const& row : encounter.rows)
        if (Creature* creature = Resolve(row))
            spread = std::max(spread, std::sqrt(encounter.origin.SqDist2d(creature->GetPositionX(), creature->GetPositionY())));
    std::string why;
    for (LeaderScaleParticipant const& member : encounter.rows.front().participants)
    {
        Player* const bot = ObjectAccessor::FindConnectedPlayer(member.guid);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!ai || ai->GetCoaEpoch() != member.epoch || bot->GetMapId() != encounter.origin.mapId)
            continue;
        if (!Projectors(bot, encounter.projectorRadius, why))
            return std::numeric_limits<float>::quiet_NaN();
        for (Unit* controlled : bot->m_Controlled)
            if (controlled && controlled->GetMap() == bot->GetMap() &&
                !Projectors(controlled, encounter.projectorRadius, why))
                return std::numeric_limits<float>::quiet_NaN();
    }
    return spread + std::max(200.0f, encounter.projectorRadius + 50.0f);
}

bool LeaderScaler::WithdrawalClear(uint32 activityId)
{
    COA_ASSERT_WORLD_THREAD();
    auto const found = Storage().encounters.find(activityId);
    if (found == Storage().encounters.end())
        return true;
    Encounter& encounter = found->second;
    std::string why;
    for (LeaderScaleParticipant const& member : encounter.rows.front().participants)
    {
        Player* const bot = ObjectAccessor::FindConnectedPlayer(member.guid);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!ai || ai->GetCoaEpoch() != member.epoch || !bot->IsInWorld())
            continue;
        if (!Projectors(bot, encounter.projectorRadius, why))
            return false;
        for (Unit* controlled : bot->m_Controlled)
            if (controlled && controlled->GetMap() == bot->GetMap() &&
                !Projectors(controlled, encounter.projectorRadius, why))
                return false;
    }
    float const distance = std::max(200.0f, encounter.projectorRadius + 50.0f);
    for (LeaderScaleParticipant const& member : encounter.rows.front().participants)
    {
        Player* const bot = ObjectAccessor::FindConnectedPlayer(member.guid);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!ai || ai->GetCoaEpoch() != member.epoch || !bot->IsInWorld())
            continue;  // the old player lifetime cannot project an aura from this disconnected identity
        if (bot->IsAlive() && (bot->GetMapId() != encounter.origin.mapId || bot->IsInCombat()))
            return false;
        if (bot->IsAlive() || HasUnitProjector(bot))
            for (LeaseRow const& row : encounter.rows)
                if (Creature* creature = Resolve(row); creature && creature->IsAlive() &&
                    bot->GetMap() == creature->GetMap() && bot->GetExactDist2d(creature) <= distance)
                    return false;
        for (Unit* controlled : bot->m_Controlled)
            if (controlled && controlled->IsInWorld() && controlled->GetMap() == bot->GetMap() &&
                HasUnitProjector(controlled))
                for (LeaseRow const& row : encounter.rows)
                    if (Creature* creature = Resolve(row); creature && creature->IsAlive() &&
                        controlled->GetExactDist2d(creature) <= distance)
                        return false;
    }
    return true;
}

void LeaderScaler::Restore(uint32 activityId)
{
    COA_ASSERT_WORLD_THREAD();
    State& state = Storage();
    auto const found = state.encounters.find(activityId);
    if (found == state.encounters.end())
        return;
    state.lastRestored = found->second.rows;
    for (LeaseRow const& row : found->second.rows)
        if (!Released(row))
        {
            if (Creature* const creature = Resolve(row))
            {
                if (creature->IsAlive() && creature->IsInCombat())
                {
                    if (creature->IsAIEnabled && creature->AI())
                        creature->AI()->EnterEvadeMode();
                    else
                        creature->CombatStop(true);
                }
                // Evade callbacks may synchronously remove the instance and restore it through the hook.
                if (!Released(row))
                    if (Creature* const current = Resolve(row))
                        RestoreCreature(current, row.original, true);
            }
        }
    state.encounters.erase(found);
    {
        std::lock_guard<std::mutex> lock(state.gate);
        // Evade/rollback callbacks above can synchronously remove a static spawn. Preserve the final
        // release facts before clearing the mailbox so a reused spawn GUID cannot pass as the old object.
        for (LeaseRow& row : state.lastRestored)
            if (auto const released = state.released.find({row.guid, row.generation}); released != state.released.end())
            {
                row.restoredDead = released->second == NoticeKind::Death || released->second == NoticeKind::ForeignDeath;
                row.restoredRemoved = released->second == NoticeKind::Remove;
            }
        state.notices.erase(std::remove_if(state.notices.begin(), state.notices.end(),
            [activityId](Notice const& notice) { return notice.activityId == activityId; }), state.notices.end());
        if (state.encounters.empty())
        {
            state.overflow = false;
            state.released.clear();
        }
    }
    Publish();
    LOG_INFO(LOGGER, "activity #{} leader scale: original runtime stats restored; dead creatures remain dead", activityId);
}

void LeaderScaler::RestoreAll()
{
    COA_ASSERT_WORLD_THREAD();
    while (!Storage().encounters.empty())
        Restore(Storage().encounters.begin()->first);
}

void LeaderScaler::OnCreatureRemoveWorld(Creature* creature) { ImmediateRestore(creature, NoticeKind::Remove); }
void LeaderScaler::OnUnitDeath(Unit* unit, Unit* killer)
{
    ImmediateRestore(unit ? unit->ToCreature() : nullptr, NoticeKind::Death, killer);
}

void LeaderScaler::OnDamage(Unit* attacker, Unit* victim, uint32& damage)
{
    if (!HasLeases() || !attacker || !victim || !damage)
        return;
    auto const snapshot = Current();
    LeaseRow const* const from = Row(attacker, *snapshot);
    LeaseRow const* const to = Row(victim, *snapshot);
    LeaseRow const* const actor = MemberLease(attacker, *snapshot);
    LeaseRow const* const member = MemberLease(victim, *snapshot);
    if ((from && from->retiring) || (to && to->retiring))
    { damage = 0; return; }
    if (!Permitted(from, to, actor, member))
    {
        damage = 0;
        Notify(*(from ? from : to ? to : actor ? actor : member), NoticeKind::Intrusion);
    }
}

bool LeaderScaler::AttackAllowed(PlayerbotAI* ai, Unit* target)
{
    if (!HasLeases() || !ai || !ai->GetBot())
        return true;
    auto const snapshot = Current();
    LeaseRow const* const member = MemberLease(ai->GetBot(), *snapshot);
    if (!member)
        return true;
    if (member->retiring)
        return false;
    return target && target->IsAlive() && SameLease(member, Row(target, *snapshot));
}

bool LeaderScaler::CanPrepare(Spell* spell)
{
    if (!HasLeases() || !spell)
        return true;
    auto const snapshot = Current();
    Unit* const caster = spell->GetCaster();
    Unit* const target = spell->m_targets.GetUnitTarget();
    LeaseRow const* const from = Row(caster, *snapshot);
    LeaseRow const* const to = Row(target, *snapshot);
    LeaseRow const* const actor = MemberLease(caster, *snapshot);
    LeaseRow const* const member = MemberLease(target, *snapshot);
    if ((from && from->retiring) || (to && to->retiring))
        return false;
    if (!from && !to && !actor && !member)
        return true;
    if (from || to)
        for (SpellEffectInfo const& effect : spell->GetSpellInfo()->Effects)
            if (!LeaderScalerRules::SpellEffectAllowed(effect.Effect))
                return false;
    // Buff/debuff aura state cannot be faithfully reconstructed by the scalar rollback.
    if (to)
        for (SpellEffectInfo const& effect : spell->GetSpellInfo()->Effects)
            if (effect.IsAura())
                return false;
    if (target && (from || to || !spell->GetSpellInfo()->IsPositive()) && !Permitted(from, to, actor, member))
        return false;
    return true;
}

void LeaderScaler::OnSpellCalculatedTarget(Spell* spell, TargetInfo& target)
{
    if (!HasLeases() || !spell || !spell->GetCaster())
        return;
    auto const snapshot = Current();
    Unit* const caster = spell->GetCaster();
    Unit* const victim = ObjectAccessor::GetUnit(*caster, target.targetGUID);
    LeaseRow const* const from = Row(caster, *snapshot);
    LeaseRow const* const to = Row(victim, *snapshot);
    LeaseRow const* const actor = MemberLease(caster, *snapshot);
    LeaseRow const* const member = MemberLease(victim, *snapshot);
    if (!from && !to && !actor && !member)
        return;
    bool const permitted = (!(from || to) && spell->GetSpellInfo()->IsPositive()) || Permitted(from, to, actor, member);
    bool aura = false;
    bool unsupported = false;
    if (from || to)
        for (SpellEffectInfo const& effect : spell->GetSpellInfo()->Effects)
        {
            aura = aura || (to && effect.IsAura());
            unsupported = unsupported || !LeaderScalerRules::SpellEffectAllowed(effect.Effect);
        }
    bool const retiring = (from && from->retiring) || (to && to->retiring);
    if (!permitted || aura || unsupported || retiring)
    {
        target.effectMask = 0;
        target.missCondition = SPELL_MISS_IMMUNE;
        target.damage = 0;
        target.damageBeforeTakenMods = 0;
        if (!permitted && !retiring)
            Notify(*(from ? from : to ? to : actor ? actor : member), NoticeKind::Intrusion);
        return;
    }
    SpellDmgClass const kind = SpellDmgClass(spell->GetSpellInfo()->DmgClass);
    if (from && (kind == SPELL_DAMAGE_CLASS_MAGIC || kind == SPELL_DAMAGE_CLASS_NONE) && target.damage > 0)
    {
        target.damage = int32(LeaderScalerRules::ScaleDamage(uint32(target.damage), from->damagePercent));
        target.damageBeforeTakenMods = int32(LeaderScalerRules::ScaleDamage(
            uint32(std::max<int32>(0, target.damageBeforeTakenMods)), from->damagePercent));
    }
}

std::vector<std::string> LeaderScaler::Describe(uint32 activityId)
{
    COA_ASSERT_WORLD_THREAD();
    auto const found = Storage().encounters.find(activityId);
    if (found == Storage().encounters.end())
        return {"leader scale: no active runtime lease"};
    return {"leader scale: " + std::to_string(found->second.rows.size()) +
        " explicit spawn instances; generation " + std::to_string(found->second.generation) +
        "; rewards inhibited; exact scalar rollback; no persistent template/SQL changes"};
}

std::vector<std::string> LeaderScaler::VerifyRestored()
{
    COA_ASSERT_WORLD_THREAD();
    if (HasLeases())
        return {"SKIP scalecheck: a runtime lease is still active; check after kill, evade or stop"};
    if (Storage().lastRestored.empty())
        return {"SKIP scalecheck: no completed runtime lease in this process"};
    std::vector<std::string> lines{"scalecheck: last completed lease, same loaded identities; no mutations"};
    uint32 passed = 0, failed = 0, skipped = 0;
    auto sameFloat = [](float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; };
    for (LeaseRow const& row : Storage().lastRestored)
    {
        std::string const label = "spawn " + std::to_string(row.spawnId) + " generation " + std::to_string(row.generation);
        Creature* const creature = Resolve(row);
        if (!creature || !creature->IsInWorld() || row.restoredRemoved || (row.restoredDead && creature->IsAlive()))
        {
            lines.push_back("SKIP " + label + ": original runtime identity is unavailable; no replacement was inspected");
            ++skipped;
            continue;
        }
        StatSnapshot const& original = row.original;
        uint32 mismatches = 0;
        std::string first;
        auto check = [&](bool ok, std::string const& field)
        {
            if (!ok)
            {
                ++mismatches;
                if (first.empty())
                    first = field;
            }
        };
        for (auto const& [index, value] : original.fields)
        {
            // Living health can change through ordinary regeneration/combat after rollback. Dead health stays zero.
            if (index == UNIT_FIELD_HEALTH)
            {
                check(creature->IsAlive() || creature->GetHealth() == 0, "dead health");
                continue;
            }
            uint32 const expected = !creature->IsAlive() && index >= UNIT_FIELD_POWER1 &&
                index < UNIT_FIELD_POWER1 + MAX_POWERS ? 0 : value;
            check(creature->GetUInt32Value(index) == expected, "update-field " + std::to_string(index));
        }
        for (uint16 mod = 0; mod < UNIT_MOD_END; ++mod)
        {
            for (uint8 type = 0; type < MODIFIER_TYPE_FLAT_END; ++type)
                check(sameFloat(creature->GetFlatModifierValue(UnitMods(mod), UnitModifierFlatType(type)),
                                original.flat[mod][type]), "flat modifier " + std::to_string(mod));
            for (uint8 type = 0; type < MODIFIER_TYPE_PCT_END; ++type)
                check(sameFloat(creature->GetPctModifierValue(UnitMods(mod), UnitModifierPctType(type)),
                                original.pct[mod][type]), "percent modifier " + std::to_string(mod));
        }
        check(creature->HasOffhandWeaponForAttack() == original.offhand, "offhand state");
        for (uint8 attack = 0; attack < MAX_ATTACK; ++attack)
        {
            if (attack == OFF_ATTACK && !original.offhand)
                continue;
            for (uint8 rangeId = 0; rangeId < MAX_WEAPON_DAMAGE_RANGE; ++rangeId)
                for (uint8 index = 0; index < MAX_ITEM_PROTO_DAMAGES; ++index)
                    check(sameFloat(creature->GetWeaponDamageRange(WeaponAttackType(attack), WeaponDamageRange(rangeId), index),
                                    original.weapon[attack][rangeId][index]), "base weapon range " + std::to_string(attack));
        }
        for (uint8 stat = 0; stat < MAX_STATS; ++stat)
            check(sameFloat(creature->GetCreateStat(Stats(stat)), original.createStats[stat]), "create stat " + std::to_string(stat));
        for (uint8 move = 0; move < MAX_MOVE_TYPE; ++move)
            check(sameFloat(creature->GetSpeedRate(UnitMoveType(move)), original.speeds[move]), "movement rate " + std::to_string(move));
        for (uint8 attack = 0; attack < MAX_ATTACK; ++attack)
            check(sameFloat(creature->m_modAttackSpeedPct[attack], original.attackSpeed[attack]), "attack speed " + std::to_string(attack));
        check(sameFloat(creature->m_modMeleeHitChance, original.hit[0]) &&
              sameFloat(creature->m_modRangedHitChance, original.hit[1]) &&
              sameFloat(creature->m_modSpellHitChance, original.hit[2]), "hit modifiers");
        check(creature->m_baseSpellCritChance == original.spellCrit, "spell crit modifier");
        check(creature->CanModifyStats() == original.modifyStats, "can-modify-stats");
        check(creature->GetLootMode() == original.lootMode && creature->IsLootRewardDisabled() == original.lootDisabled &&
              creature->IsReputationRewardDisabled() == original.reputationDisabled, "reward flags");
        check(creature->GetAppliedAuras().empty(), "original empty aura state");
        check(Immunities(creature) == original.immunities, "native immunity metadata / owned tag removal");
        lines.push_back(std::string(mismatches ? "FAIL " : "PASS ") + label + ": " +
            (mismatches ? std::to_string(mismatches) + " scalar mismatches, first " + first : "captured scalar bits/modifiers/speeds/reward flags restored"));
        if (mismatches)
            ++failed;
        else
            ++passed;
    }
    lines.push_back("scalecheck: " + std::to_string(passed) + " passed, " + std::to_string(failed) + " failed, " +
                    std::to_string(skipped) + " unavailable identities; kill/evade/restart coverage depends on the preceding test");
    return lines;
}

std::vector<std::string> LeaderScalerRules::SelfTest()
{
    uint32 failures = 0;
    uint32 count = 0;
    auto check = [&](bool value) { ++count; failures += value ? 0 : 1; };
    check(ScaleDamage(1000, 10) == 100);
    check(ScaleDamage(1, 1) == 0);
    check(ScaleDamage(UINT32_MAX, 100) == UINT32_MAX);
    check(ScaleDamage(UINT32_MAX, 1) == UINT32_MAX / 100);
    check(ScaleDamage(123, 1000) == 123);
    check(ScaleDamage(0, 10) == 0);
    LeaderScaleParameters parameters;
    check(Valid(parameters));
    parameters.level = 0; check(!Valid(parameters));
    parameters.level = 61; check(!Valid(parameters));
    parameters.level = 60;
    parameters.health = 0; check(!Valid(parameters));
    parameters.health = UINT32_MAX; check(!Valid(parameters));
    parameters.health = 1;
    parameters.guardHealth = 0; check(!Valid(parameters));
    parameters.guardHealth = 1;
    parameters.damagePercent = 0; check(!Valid(parameters));
    parameters.damagePercent = 101; check(!Valid(parameters));
    parameters.damagePercent = 100;
    parameters.guardDamagePercent = 101; check(!Valid(parameters));
    parameters.guardDamagePercent = 100;
    parameters.armorPercent = 0; check(!Valid(parameters));
    parameters.armorPercent = 100; check(Valid(parameters));
    check(SpellEffectAllowed(SPELL_EFFECT_SCHOOL_DAMAGE));
    check(SpellEffectAllowed(SPELL_EFFECT_WEAPON_DAMAGE));
    check(SpellEffectAllowed(SPELL_EFFECT_HEAL));
    check(SpellEffectAllowed(SPELL_EFFECT_TRIGGER_SPELL));
    check(!SpellEffectAllowed(SPELL_EFFECT_DUMMY));
    check(!SpellEffectAllowed(SPELL_EFFECT_SCRIPT_EFFECT));
    check(!SpellEffectAllowed(SPELL_EFFECT_SUMMON));
    check(!SpellEffectAllowed(SPELL_EFFECT_PERSISTENT_AREA_AURA));
    check(!SpellEffectAllowed(SPELL_EFFECT_KILL_CREDIT));
    check(!SpellEffectAllowed(SPELL_EFFECT_TELEPORT_UNITS));
    check(!SpellEffectAllowed(SPELL_EFFECT_INSTAKILL));
    check(!SpellEffectAllowed(198));
    LeaseRow original;
    original.activityId = 1;
    original.generation = 1;
    LeaseRow same = original;
    LeaseRow other = original;
    other.activityId = 2;
    LeaseRow replacement = original;
    replacement.generation = 2;
    check(Permitted(nullptr, nullptr, nullptr, nullptr));
    check(Permitted(&original, nullptr, nullptr, &same));
    check(Permitted(nullptr, &original, &same, nullptr));
    check(Permitted(&original, &same, nullptr, nullptr));
    check(!Permitted(&original, nullptr, nullptr, nullptr));
    check(!Permitted(nullptr, &original, nullptr, nullptr));
    check(!Permitted(nullptr, nullptr, &original, nullptr));
    check(!Permitted(nullptr, nullptr, nullptr, &original));
    check(!Permitted(&original, &other, nullptr, nullptr));
    check(!Permitted(&original, nullptr, nullptr, &replacement));
    // Native owner/pet/summon projections cannot reach an unowned static lease; enemy/friendly area
    // projections can. In particular a pet-scaling OWNER_POWER_SCALING aura must not block all admissions.
    check(ReachesUnownedNpc(SPELL_EFFECT_APPLY_AREA_AURA_ENEMY));
    check(ReachesUnownedNpc(SPELL_EFFECT_APPLY_AREA_AURA_FRIEND));
    check(ReachesUnownedNpc(SPELL_EFFECT_APPLY_AREA_AURA_PARTY));
    check(ReachesUnownedNpc(SPELL_EFFECT_APPLY_AREA_AURA_RAID));
    check(ReachesUnownedNpc(SPELL_EFFECT_PERSISTENT_AREA_AURA));
    check(!ReachesUnownedNpc(SPELL_EFFECT_APPLY_AREA_AURA_OWNER));
    check(!ReachesUnownedNpc(SPELL_EFFECT_APPLY_AREA_AURA_PET));
    check(!ReachesUnownedNpc(SPELL_EFFECT_ASCENSION_APPLY_AURA_TO_SUMMONS));
    check(!ReachesUnownedNpc(SPELL_EFFECT_APPLY_AURA));
    return {std::string(failures ? "FAIL " : "PASS ") + "LeaderScaler pure rules " + std::to_string(count) +
        " checks, " + std::to_string(failures) + " failed; runtime rollback/GM kill not exercised"};
}
