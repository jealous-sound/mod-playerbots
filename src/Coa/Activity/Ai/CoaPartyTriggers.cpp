/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaPartyTriggers.h"
#include "ActivityRegistry.h"
#include "CoaActivityValues.h"
#include "CoaTime.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include <iterator>

namespace
{
constexpr float PARTY_GREET_YARDS = 12.0f;
constexpr uint64 PARTY_GREET_REPEAT_MS = 10 * 60 * 1000;  // a bot greets the same player at most this often
constexpr uint32 PARTY_GREET_SOCIABILITY = 40;            // shyer bots never greet
constexpr uint64 GREET_SCAN_MS = 5 * 1000;
constexpr uint64 MOUNT_TRAIT_SALT = 0x40017;              // ActivityTraits::Trait below 50: shows its mount
constexpr uint32 MOUNT_TRAIT_BELOW = 50;

// The row of an Active party, loiter or gurubashi participant that is alive, out of combat and not on a taxi.
ActivityMemberView const* PartyRow(Player* bot, ActivityBotState const& s)
{
    ActivityMemberView const* r = s.row;
    if (!r || r->phase != ActivityPhase::Active)
        return nullptr;
    if (r->kind != ActivityKindId::Party && r->kind != ActivityKindId::Loiter && r->kind != ActivityKindId::Gurubashi)
        return nullptr;
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight())
        return nullptr;
    return r;
}
}  // namespace

bool CoaPartyEmoteTrigger::IsActive()
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    if (!PartyRow(bot, s) || bot->isMoving())
        return false;

    uint64 const now = CoaTime::NowMs();
    // The first emote of this life of the bot comes a little later, so a crowd does not emote all at once.
    if (!s.nextPartyEmoteMs)
    {
        s.nextPartyEmoteMs = now + urand(3000, 15000);
        return false;
    }
    return now >= s.nextPartyEmoteMs;
}

bool CoaPartyGreetTrigger::IsActive()
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    ActivityMemberView const* r = PartyRow(bot, s);
    if (!r || (r->style != ACTIVITY_STYLE_INN && r->style != ACTIVITY_STYLE_DANCE) ||
        ActivityTraits::Sociability(bot->GetGUID().GetCounter()) < PARTY_GREET_SOCIABILITY)
    {
        s.greetTarget.Clear();
        return false;
    }

    // Someone found on an earlier scan: the action greets it.
    if (!s.greetTarget.IsEmpty())
        return true;

    uint64 const now = CoaTime::NowMs();
    if (now < s.nextGreetScanMs)
        return false;
    s.nextGreetScanMs = now + GREET_SCAN_MS;

    for (auto it = s.greetedMs.begin(); it != s.greetedMs.end();)
        it = now >= it->second + PARTY_GREET_REPEAT_MS ? s.greetedMs.erase(it) : std::next(it);

    GuidVector const nearby = AI_VALUE(GuidVector, "nearest friendly players");
    for (ObjectGuid const& guid : nearby)
    {
        Player* p = ObjectAccessor::GetPlayer(*bot, guid);
        if (!p || p == bot || !p->IsAlive() || p->IsGameMaster() || !CoaActivityBot::HasRealSession(p))
            continue;
        if (s.greetedMs.count(guid.GetCounter()) || bot->GetExactDist(p) > PARTY_GREET_YARDS ||
            !bot->IsWithinLOSInMap(p))
            continue;

        s.greetTarget = guid;
        return true;
    }
    return false;
}

bool CoaPartyMountTrigger::IsActive()
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    ActivityMemberView const* r = PartyRow(bot, s);
    if (!r || r->style != ACTIVITY_STYLE_LOITER || bot->IsMounted() || bot->isMoving())
        return false;
    if (ActivityTraits::Trait(bot->GetGUID().GetCounter(), MOUNT_TRAIT_SALT) >= MOUNT_TRAIT_BELOW)
        return false;

    return CoaActivityBot::DistanceToCenter(bot, *r) <= r->radius && CoaTime::NowMs() >= s.nextMountMs;
}
