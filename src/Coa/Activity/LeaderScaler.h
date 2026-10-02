/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_LEADERSCALER_H
#define PLAYERBOTS_LEADERSCALER_H

#include "Define.h"
#include "ObjectGuid.h"
#include "ActivityTypes.h"
#include <string>
#include <vector>

class Creature;
class Spell;
class Unit;
class PlayerbotAI;
struct TargetInfo;

struct LeaderScaleParameters
{
    uint8 level = 60;
    uint32 health = 250000;
    uint32 guardHealth = 15000;
    uint32 damagePercent = 10;
    uint32 guardDamagePercent = 20;
    uint32 armorPercent = 25;
};

struct LeaderScaleParticipant
{
    ObjectGuid guid;
    uint64 epoch = 0;
};

enum class LeaderScaleResult : uint8
{
    Active = 0,
    Killed,
    Evaded,
    Lost,
    Changed,
    Restored
};

namespace LeaderScalerRules
{
uint32 ScaleDamage(uint32 damage, uint32 percent);
bool Valid(LeaderScaleParameters const& parameters);
bool SpellEffectAllowed(uint32 effect);
std::vector<std::string> SelfTest();
}

// Reversible, process-local runtime leases of explicit static spawn instances. No templates or SQL are changed.
// Begin/Poll/Restore/RestoreAll run on the director's world thread after map workers join. Other entries run on
// the provided creature/unit's owning map thread and read an immutable snapshot; notifications retain GUIDs only.
class LeaderScaler
{
public:
    static bool Begin(uint32 activityId, Creature* leader, std::vector<Creature*> const& guards,
                      std::vector<LeaderScaleParticipant> const& participants,
                      LeaderScaleParameters const& parameters, std::string& why,
                      ActivityKindId kind = ActivityKindId::LeaderEncounter);
    static LeaderScaleResult Poll(uint32 activityId, std::string& why);
    // Keep aura protection while the raid walks away. Returns the required radius from the captured leader
    // position, including every current leased spawn and the participants' active area-aura radii.
    static float BeginWithdrawal(uint32 activityId);
    static bool WithdrawalClear(uint32 activityId);
    static void Restore(uint32 activityId);
    static void RestoreAll();
    static bool HasLeases();
    static std::vector<std::string> Describe(uint32 activityId);
    // Last completed lease only (at most 41 scalar snapshots); unavailable identities are SKIP, never replaced.
    static std::vector<std::string> VerifyRestored();

    // Hook registration belongs to CoaHooks. Removal/death synchronously restore the provided NPC's stats before
    // its object can disappear, then notify Poll without retaining the object. A killed NPC keeps zero health.
    static void OnCreatureRemoveWorld(Creature* creature);
    static void OnUnitDeath(Unit* unit, Unit* killer = nullptr);
    static void OnDamage(Unit* attacker, Unit* victim, uint32& damage);
    static bool AttackAllowed(PlayerbotAI* ai, Unit* target);
    static bool CanPrepare(Spell* spell);
    static void OnSpellCalculatedTarget(Spell* spell, TargetInfo& target);
};

#endif
