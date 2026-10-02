/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_ACTIVITYPVPPOLICY_H
#define PLAYERBOTS_ACTIVITYPVPPOLICY_H
#include "ActivityTypes.h"
#include "ObjectGuid.h"
class Player;
class PlayerbotAI;
class Unit;
class Spell;
class SpellInfo;
namespace ActivityPvpPolicy
{
bool Controlled(ActivityKindId kind);
bool Active(PlayerbotAI* ai);
bool Eligible(Player* attacker, Player* target, uint32 ruleset, bool nativeCheck = true);
bool AttackAllowed(PlayerbotAI* ai, Unit* target);
bool CastAllowed(PlayerbotAI* ai, SpellInfo const* spell, Unit* target);
bool CanPrepare(Spell* spell);
void Update(PlayerbotAI* ai);
void OnDamage(Unit* attacker, Unit* victim, uint32& damage);
void OnDeath(Unit* victim, Unit* killer);
bool Killed(uint32 activityId, ObjectGuid victim);
void Release(uint32 activityId);
}
#endif
