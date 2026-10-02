/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYPVEPOLICY_H
#define PLAYERBOTS_ACTIVITYPVEPOLICY_H

#include "ActivityTypes.h"
#include <string>
#include <vector>

class Player;
class PlayerbotAI;
class Unit;
class Creature;
class SpellInfo;
class Spell;
struct TargetInfo;

namespace ActivityPvePolicy
{
bool Protected(ActivityKindId kind);
bool TargetAllowed(ActivityKindId kind, bool safe, bool playerControlled, uint32 creatureRank);
bool SpellAllowed(ActivityKindId kind, bool safe, bool positive, bool area, bool targetAllowed);
bool SafePlayer(Player* player, bool allowInstance = false);
bool RareSafe(Player* player);
bool AttackAllowed(PlayerbotAI* ai, Unit* target);
bool CastAllowed(PlayerbotAI* ai, SpellInfo const* spell, Unit* target);
bool LootAllowed(PlayerbotAI* ai);
void Update(PlayerbotAI* ai);
void Enroll(PlayerbotAI* ai, ActivityKindId kind);
void Release(PlayerbotAI* ai);
void PreparePetSave(PlayerbotAI* ai);
void OnCreatureAddWorld(Creature* creature);
void OnDamage(Unit* attacker, Unit* victim, uint32& damage);
bool CanPrepare(Spell* spell);
void GuardReflection(Spell* spell, TargetInfo& target);
std::vector<std::string> SelfTest();
}

#endif
