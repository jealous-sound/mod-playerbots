/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaMemoryTriggers.h"
#include "CoaMemoryRules.h"
#include "CoaMemoryValues.h"
#include "CoaPlayerPrefs.h"
#include "Group.h"
#include "Log.h"
#include "MemoryView.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotSecurity.h"
#include "Playerbots.h"
#include "WorldSession.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <vector>

namespace
{
constexpr uint32 GREET_BOT_COOLDOWN = 120;    // a bot greets by memory at most every 2 minutes
constexpr uint32 GREET_PAIR_COOLDOWN = 3600;  // and each player at most once an hour
constexpr uint32 GREET_AFTER_CONTACT = 300;   // not right after a remembered interaction
constexpr float GREET_RANGE = 20.0f;
constexpr uint32 OFFER_BOT_COOLDOWN = 1800;
constexpr uint32 OFFER_PAIR_COOLDOWN = 6 * 3600;
constexpr float OFFER_RANGE = 30.0f;
constexpr int32 OFFER_LEVEL_SPREAD = 5;
constexpr uint8 GREET_RANK_NONE = 0xFF;

// `seconds` passed since `at` (0 = never). A clock set back counts as passed: a cooldown never locks up.
bool Elapsed(uint32 now, uint32 at, uint32 seconds)
{
    return !at || now < at || now - at >= seconds;
}

uint32 LastAt(std::unordered_map<uint32, uint32> const& stamps, uint32 low)
{
    auto const found = stamps.find(low);
    return found == stamps.end() ? 0 : found->second;
}

// The bot may turn to someone: alive, calm, on its feet, outside battlegrounds.
bool BotIsFree(Player* bot)
{
    return bot->IsAlive() && !bot->IsInCombat() && !bot->InBattleground() && !bot->IsBeingTeleported() &&
           !bot->GetVehicle() && !bot->IsInFlight() && !bot->IsFlying();
}

// A real player on the bot's map within `range`, alive, out of combat, not a GM. Null otherwise.
Player* RealPlayerNear(Player* bot, ObjectGuid guid, float range, float& distance)
{
    Player* player = ObjectAccessor::GetPlayer(*bot, guid);
    if (!player || player == bot)
        return nullptr;

    WorldSession* session = player->GetSession();
    if (!session || session->IsBot() || player->IsGameMaster() || !player->IsAlive() || player->IsInCombat())
        return nullptr;

    distance = bot->GetDistance(player);
    return distance <= range ? player : nullptr;
}

// Who is greeted first when several known players come by.
uint8 GreetRank(MemoryLabel label)
{
    switch (label)
    {
        case MemoryLabel::Friend:
            return 0;
        case MemoryLabel::Grudge:
            return 1;
        case MemoryLabel::Rival:
            return 2;
        case MemoryLabel::Acquaintance:
            return 3;
        default:
            return GREET_RANK_NONE;
    }
}

struct Candidate
{
    Player* player;
    MemoryLabel label;
    uint8 rank;
    float distance;
};
}  // namespace

bool CoaKnownPlayerNearTrigger::IsActive()
{
    std::shared_ptr<MemoryView const> const view = CoaMemoryRules::ViewFor(botAI, MemoryBehaviour::GreetByMemory);
    if (!view)
        return false;

    CoaMemoryBotState& state = AI_VALUE(CoaMemoryBotState&, "coa memory state");
    state.greetTarget.Clear();

    uint32 const now = uint32(time(nullptr));
    CoaPlayerPrefs const& prefs = CoaPlayerPrefs::Instance();
    if (!BotIsFree(bot) || !Elapsed(now, state.lastGreetAt, GREET_BOT_COOLDOWN) || !prefs.IsLoaded())
        return false;

    // The bot's master and group are company, not people coming by (IsMember covers every subgroup of a raid).
    Group* const group = bot->GetGroup();
    Player* const master = botAI->GetMaster();
    std::vector<Candidate> candidates;
    GuidVector const players = AI_VALUE(GuidVector, "nearest friendly players");
    for (ObjectGuid const& guid : players)
    {
        float distance = 0.0f;
        Player* player = RealPlayerNear(bot, guid, GREET_RANGE, distance);
        if (!player || player == master || (group && group->IsMember(guid)))
            continue;

        uint32 const low = guid.GetCounter();
        MemoryRelationView const* relation = view->Find(low);
        if (!relation || !Elapsed(now, relation->lastSeen, GREET_AFTER_CONTACT) ||
            !Elapsed(now, LastAt(state.greetedAt, low), GREET_PAIR_COOLDOWN))
            continue;

        MemoryLabel const label = view->Label(*relation, now);
        uint8 const rank = GreetRank(label);
        if (rank == GREET_RANK_NONE)
            continue;

        CoaPrefs const playerPrefs = prefs.Get(player->GetSession()->GetAccountId());
        if (playerPrefs.VoiceOptedOut() || playerPrefs.MemoryOptedOut() ||
            (label == MemoryLabel::Grudge && (playerPrefs.activityMask & COA_OPTOUT_HOSTILE)))
            continue;

        candidates.push_back({player, label, rank, distance});
    }

    std::sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b)
              { return a.rank != b.rank ? a.rank < b.rank : a.distance < b.distance; });

    // Line of sight last: it is the costly test.
    for (Candidate const& candidate : candidates)
    {
        if (!bot->IsWithinLOSInMap(candidate.player))
            continue;

        state.greetTarget = candidate.player->GetGUID();
        state.greetLabel = candidate.label;
        LOG_DEBUG("playerbots.memory", "{} will greet {} ({})", bot->GetName(), candidate.player->GetName(),
                  MemoryLabelName(candidate.label));
        return true;
    }

    return false;
}

bool CoaFriendNearTrigger::IsActive()
{
    std::shared_ptr<MemoryView const> const view = CoaMemoryRules::ViewFor(botAI, MemoryBehaviour::FriendInvites);
    if (!view)
        return false;

    CoaMemoryBotState& state = AI_VALUE(CoaMemoryBotState&, "coa memory state");
    state.offerTarget.Clear();

    uint32 const now = uint32(time(nullptr));
    CoaPlayerPrefs const& prefs = CoaPlayerPrefs::Instance();
    if (!BotIsFree(bot) || bot->GetGroup() || botAI->GetMaster() ||
        !Elapsed(now, state.lastOfferAt, OFFER_BOT_COOLDOWN) || !prefs.IsLoaded())
        return false;

    std::vector<Candidate> candidates;
    GuidVector const players = AI_VALUE(GuidVector, "nearest friendly players");
    for (ObjectGuid const& guid : players)
    {
        float distance = 0.0f;
        Player* player = RealPlayerNear(bot, guid, OFFER_RANGE, distance);
        if (!player || player->GetGroup() ||
            std::abs(int32(player->GetLevel()) - int32(bot->GetLevel())) > OFFER_LEVEL_SPREAD)
            continue;

        uint32 const low = guid.GetCounter();
        MemoryRelationView const* relation = view->Find(low);
        if (!relation || !Elapsed(now, LastAt(state.offeredAt, low), OFFER_PAIR_COOLDOWN) ||
            view->Label(*relation, now) != MemoryLabel::Friend)
            continue;

        CoaPrefs const playerPrefs = prefs.Get(player->GetSession()->GetAccountId());
        if (playerPrefs.VoiceOptedOut() || playerPrefs.MemoryOptedOut() ||
            (playerPrefs.activityMask & COA_OPTOUT_WHISPERS))
            continue;

        candidates.push_back({player, MemoryLabel::Friend, 0, distance});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](Candidate const& a, Candidate const& b) { return a.distance < b.distance; });

    // The nearest one the bot would accept an invite from (the same rule AcceptInvitationAction applies).
    for (Candidate const& candidate : candidates)
    {
        if (botAI->GetSecurity()->LevelFor(candidate.player) < PLAYERBOT_SECURITY_INVITE ||
            !bot->IsWithinLOSInMap(candidate.player))
            continue;

        state.offerTarget = candidate.player->GetGUID();
        LOG_DEBUG("playerbots.memory", "{} will offer a group to {}", bot->GetName(), candidate.player->GetName());
        return true;
    }

    return false;
}
