/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RFCTriggers.h"
#include "AiObjectContext.h"
#include "Playerbots.h"

namespace RagefireEscort
{
    Unit* Nearest(PlayerbotAI* botAI, Player* bot, AiObjectContext* context, std::string const& bossName,
                  std::initializer_list<uint32> entries)
    {
        Unit* boss = AI_VALUE2(Unit*, "find target", bossName);
        if (!boss)
            return nullptr;

        GuidVector targets = AI_VALUE(GuidVector, "possible targets no los");
        for (uint32 entry : entries)
        {
            for (auto& guid : targets)
            {
                Unit* unit = botAI->GetUnit(guid);
                if (!unit || unit->GetEntry() != entry || !unit->IsAlive() || !unit->isTargetableForAttack())
                    continue;

                if (unit->GetExactDist2d(boss->GetPosition()) <= RFC_ESCORT_RANGE)
                    return unit;
            }
        }

        return nullptr;
    }

    bool StillUp(PlayerbotAI* botAI, Player* bot, AiObjectContext* context, std::string const& bossName,
                 std::initializer_list<uint32> entries)
    {
        if (botAI->IsTank(bot))
            return false;

        return Nearest(botAI, bot, context, bossName, entries) != nullptr;
    }
}

bool OggleflintAddsTrigger::IsActive()
{
    // The Shaman is named first on purpose: it is the one that heals, and the action takes the
    // first entry it finds alive.
    return RagefireEscort::StillUp(botAI, bot, context, "oggleflint",
                                   {NPC_RAGEFIRE_SHAMAN, NPC_RAGEFIRE_TROGG});
}

bool JergoshAddsTrigger::IsActive()
{
    return RagefireEscort::StillUp(botAI, bot, context, "jergosh the invoker",
                                   {NPC_SEARING_BLADE_CULTIST});
}

bool BazzalanDpsTrigger::IsActive()
{
    // Bazzalan is the opposite case: he hits far harder than his Cultists, so he goes down first.
    Unit* boss = AI_VALUE2(Unit*, "find target", "bazzalan");
    if (!boss || !boss->isTargetableForAttack())
        return false;

    return !botAI->IsTank(bot);
}

bool TaragamanFireNovaTrigger::IsActive()
{
    if (!botAI->IsRanged(bot))
        return false;

    Unit* boss = AI_VALUE2(Unit*, "find target", "taragaman the hungerer");
    if (!boss || !boss->IsAlive())
        return false;

    return bot->GetExactDist2d(boss->GetPosition()) < FIRE_NOVA_RADIUS + FIRE_NOVA_SAFETY;
}
