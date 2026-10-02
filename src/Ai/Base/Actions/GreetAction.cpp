/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GreetAction.h"
#include "CoaMemoryConfig.h"
#include "CoaMemoryValues.h"
#include "Event.h"
#include "Playerbots.h"
#include <ctime>

GreetAction::GreetAction(PlayerbotAI* botAI) : Action(botAI, "greet") {}

bool GreetAction::Execute(Event /*event*/)
{
    ObjectGuid guid = AI_VALUE(ObjectGuid, "new player nearby");
    if (!guid || !guid.IsPlayer())
        return false;

    // CoA memory (GreetByMemory): "coa greet known" greeted this player within the hour. Switch off: legacy.
    if (CoaMemoryConfig::BehaviourActive(MemoryBehaviour::GreetByMemory) &&
        botAI->HasStrategy("coa memory", BOT_STATE_NON_COMBAT))
    {
        CoaMemoryBotState& state = AI_VALUE(CoaMemoryBotState&, "coa memory state");
        auto const greeted = state.greetedAt.find(guid.GetCounter());
        uint32 const now = uint32(time(nullptr));
        if (greeted != state.greetedAt.end() && now >= greeted->second && now - greeted->second < 3600)
        {
            AI_VALUE(GuidSet&, "already seen players").insert(guid);
            return false;
        }
    }

    Player* player = dynamic_cast<Player*>(botAI->GetUnit(guid));
    if (!player)
        return false;

    if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, player, sPlayerbotAIConfig.sightDistance))
        bot->SetFacingToObject(player);

    ObjectGuid oldSel = bot->GetTarget();
    bot->SetTarget(guid);
    // bot->HandleEmote(EMOTE_ONESHOT_WAVE);
    botAI->PlayEmote(TEXT_EMOTE_HELLO);
    bot->SetTarget(oldSel);

    GuidSet& alreadySeenPlayers = botAI->GetAiObjectContext()->GetValue<GuidSet&>("already seen players")->Get();
    alreadySeenPlayers.insert(guid);

    GuidVector nearestPlayers = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest friendly players")->Get();
    for (ObjectGuid const guid : nearestPlayers)
    {
        alreadySeenPlayers.insert(guid);
    }

    return true;
}
