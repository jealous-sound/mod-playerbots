/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RFCTRIGGERS_H
#define PLAYERBOTS_RFCTRIGGERS_H

#include "DungeonStrategyUtils.h"
#include "GenericTriggers.h"
#include "PlayerbotAIConfig.h"
#include "Trigger.h"

enum RagefireChasmIDs
{
    NPC_OGGLEFLINT                 = 11517,
    NPC_JERGOSH_THE_INVOKER        = 11518,
    NPC_BAZZALAN                   = 11519,
    NPC_TARAGAMAN_THE_HUNGERER     = 11520,

    NPC_RAGEFIRE_TROGG             = 11318,
    NPC_RAGEFIRE_SHAMAN            = 11319,
    NPC_SEARING_BLADE_CULTIST      = 11322,

    // Taragaman's only area spell. Its radius is what the ranged have to clear.
    SPELL_FIRE_NOVA                = 11970
};

// Fire Nova reaches ten yards from Taragaman himself. The extra yards keep a bot that drifts
// slightly from being clipped by the next one.
constexpr float FIRE_NOVA_RADIUS = 10.0f;
constexpr float FIRE_NOVA_SAFETY = 3.0f;

// The room a boss shares with its escort, measured from the spawns: Oggleflint stands within
// thirteen yards of a Trogg and a Shaman, Jergosh and Bazzalan within twelve of their Cultists.
constexpr float RFC_ESCORT_RANGE = 25.0f;

// The escort is not on anyone's threat table before it is pulled, so it cannot be found the way a
// boss is. Walk the units the bot can see instead, as the Frost Tomb trigger does in Utgarde. The
// entries are tried in order, so the caller decides what dies first.
namespace RagefireEscort
{
    Unit* Nearest(PlayerbotAI* botAI, Player* bot, AiObjectContext* context, std::string const& bossName,
                  std::initializer_list<uint32> entries);

    bool StillUp(PlayerbotAI* botAI, Player* bot, AiObjectContext* context, std::string const& bossName,
                 std::initializer_list<uint32> entries);
}

class OggleflintAddsTrigger : public Trigger
{
public:
    OggleflintAddsTrigger(PlayerbotAI* ai) : Trigger(ai, "oggleflint adds") {}
    bool IsActive() override;
};

class JergoshAddsTrigger : public Trigger
{
public:
    JergoshAddsTrigger(PlayerbotAI* ai) : Trigger(ai, "jergosh adds") {}
    bool IsActive() override;
};

class BazzalanDpsTrigger : public Trigger
{
public:
    BazzalanDpsTrigger(PlayerbotAI* ai) : Trigger(ai, "bazzalan priority") {}
    bool IsActive() override;
};

class TaragamanFireNovaTrigger : public Trigger
{
public:
    TaragamanFireNovaTrigger(PlayerbotAI* ai) : Trigger(ai, "taragaman fire nova") {}
    bool IsActive() override;
};

#endif
