/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaMemoryActions.h"
#include "CoaMemoryValues.h"
#include "Event.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include <ctime>
#include <string>

namespace
{
constexpr float GREET_REACH = 25.0f;  // the trigger picks within 20 yards; the player may have walked on a little
constexpr float OFFER_REACH = 30.0f;
constexpr uint32 GREET_PAIR_COOLDOWN = 3600;
constexpr uint32 OFFER_PAIR_COOLDOWN = 6 * 3600;
constexpr std::size_t STAMPS_PRUNE_AT = 256;

// "Kegarink Bot" -> "Kegarink", as the voice director fills %player.
std::string FirstName(std::string const& name)
{
    return name.substr(0, name.find(' '));
}

// Drops the stamps whose cooldown is over once the map grows: a bot meets many players over a long session.
void PruneStamps(std::unordered_map<uint32, uint32>& stamps, uint32 now, uint32 cooldown)
{
    if (stamps.size() < STAMPS_PRUNE_AT)
        return;

    for (auto it = stamps.begin(); it != stamps.end();)
    {
        if (now < it->second || now - it->second < cooldown)
            ++it;
        else
            it = stamps.erase(it);
    }
}

struct Greeting
{
    uint32 emote;
    char const* name;
    char const* english;
};

Greeting GreetingFor(MemoryLabel label)
{
    switch (label)
    {
        case MemoryLabel::Friend:
            return {TEXT_EMOTE_WAVE, "coa_mem_greet_friend", "Hey %player, good to see you again!"};
        case MemoryLabel::Grudge:
            return {TEXT_EMOTE_GLARE, "coa_mem_greet_grudge", "%player. Again."};
        case MemoryLabel::Rival:
            return {TEXT_EMOTE_BOW, "coa_mem_greet_known", "Hello again, %player."};
        default:
            return {TEXT_EMOTE_NOD, "coa_mem_greet_known", "Hello again, %player."};
    }
}
}  // namespace

bool CoaGreetKnownAction::isUseful()
{
    return !AI_VALUE(CoaMemoryBotState&, "coa memory state").greetTarget.IsEmpty();
}

bool CoaGreetKnownAction::Execute(Event /*event*/)
{
    CoaMemoryBotState& state = AI_VALUE(CoaMemoryBotState&, "coa memory state");
    ObjectGuid const guid = state.greetTarget;
    MemoryLabel const label = state.greetLabel;
    state.greetTarget.Clear();

    Player* player = guid ? ObjectAccessor::GetPlayer(*bot, guid) : nullptr;
    if (!player || !player->IsAlive() || bot->GetDistance(player) > GREET_REACH)
        return false;

    // Facing and emote as GreetAction does them.
    if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, player, sPlayerbotAIConfig.sightDistance))
        bot->SetFacingToObject(player);

    Greeting const greeting = GreetingFor(label);
    ObjectGuid const oldSelection = bot->GetTarget();
    bot->SetTarget(guid);
    botAI->PlayEmote(greeting.emote);
    bot->SetTarget(oldSelection);

    std::string const text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        greeting.name, greeting.english, {{"%player", FirstName(player->GetName())}});
    if (!text.empty())
        botAI->Say(text);

    uint32 const now = uint32(time(nullptr));
    PruneStamps(state.greetedAt, now, GREET_PAIR_COOLDOWN);
    state.greetedAt[guid.GetCounter()] = now;
    state.lastGreetAt = now;

    // The stock greeting stays quiet for this player.
    AI_VALUE(GuidSet&, "already seen players").insert(guid);

    LOG_DEBUG("playerbots.memory", "{} greeted {} ({})", bot->GetName(), player->GetName(), MemoryLabelName(label));
    return true;
}

bool CoaOfferGroupAction::isUseful()
{
    return !AI_VALUE(CoaMemoryBotState&, "coa memory state").offerTarget.IsEmpty();
}

bool CoaOfferGroupAction::Execute(Event /*event*/)
{
    CoaMemoryBotState& state = AI_VALUE(CoaMemoryBotState&, "coa memory state");
    ObjectGuid const guid = state.offerTarget;
    state.offerTarget.Clear();

    Player* player = guid ? ObjectAccessor::GetPlayer(*bot, guid) : nullptr;
    if (!player || !player->IsAlive() || player->IsInCombat() || bot->IsInCombat() || player->GetGroup() ||
        bot->GetGroup() || bot->GetDistance(player) > OFFER_REACH)
        return false;

    std::string const text = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "coa_mem_invite_offer", "Hey %player, want to group up? Invite me if you like.",
        {{"%player", FirstName(player->GetName())}});
    if (text.empty())
        return false;

    bot->Whisper(text, LANG_UNIVERSAL, player);

    uint32 const now = uint32(time(nullptr));
    PruneStamps(state.offeredAt, now, OFFER_PAIR_COOLDOWN);
    state.offeredAt[guid.GetCounter()] = now;
    state.lastOfferAt = now;

    LOG_DEBUG("playerbots.memory", "{} offered a group to {}", bot->GetName(), player->GetName());
    return true;
}
