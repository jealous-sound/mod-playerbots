/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcceptDuelAction.h"
#include "CoaMemoryRules.h"
#include "Event.h"
#include "Playerbots.h"

bool AcceptDuelAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());

    ObjectGuid flagGuid;
    p >> flagGuid;
    ObjectGuid playerGuid;
    p >> playerGuid;

    // CoA memory (RivalDuels): Legacy with memory off, which keeps the rule below as it was (90 %).
    CoaDuelVerdict const verdict = CoaMemoryRules::DuelVerdict(botAI, playerGuid);
    uint8 const minHealth = verdict == CoaDuelVerdict::Eager ? uint8(MemoryThreshold::DUEL_EAGER_HEALTH) : uint8(90);

    // Do not auto duel with low health
    if (verdict == CoaDuelVerdict::Refuse ||
        ((!botAI->HasGameClientMaster() || (botAI->GetMaster() && botAI->GetMaster()->GetGUID() != playerGuid)) &&
         AI_VALUE2(uint8, "health", "self target") < minHealth))
    {
        WorldPacket packet(CMSG_DUEL_CANCELLED, 8);
        packet << flagGuid;
        bot->GetSession()->HandleDuelCancelledOpcode(packet);
        return false;
    }

    WorldPacket packet(CMSG_DUEL_ACCEPTED, 8);
    packet << flagGuid;
    bot->GetSession()->HandleDuelAcceptedOpcode(packet);

    botAI->ResetStrategies();
    return true;
}
