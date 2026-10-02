/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaPartyActions.h"
#include "ActivityRegistry.h"
#include "CheckMountStateAction.h"
#include "CoaActivityValues.h"
#include "CoaTime.h"
#include "EmoteAction.h"
#include "Event.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <array>
#include <vector>

namespace
{
constexpr float PARTY_FACE_YARDS = 15.0f;
constexpr uint64 MOUNT_RETRY_MS = 60 * 1000;

constexpr std::array<uint32, 6> INN_EMOTES = {TEXT_EMOTE_DRINK, TEXT_EMOTE_TOAST,   TEXT_EMOTE_LAUGH,
                                              TEXT_EMOTE_CHEER, TEXT_EMOTE_APPLAUD, TEXT_EMOTE_BOW};
constexpr std::array<uint32, 5> DANCE_EMOTES = {TEXT_EMOTE_DANCE, TEXT_EMOTE_DANCE, TEXT_EMOTE_DANCE, TEXT_EMOTE_CHEER,
                                                TEXT_EMOTE_APPLAUD};
constexpr std::array<uint32, 5> LOITER_EMOTES = {TEXT_EMOTE_BORED, TEXT_EMOTE_YAWN, TEXT_EMOTE_FLEX, TEXT_EMOTE_WAVE,
                                                 TEXT_EMOTE_LAUGH};
constexpr std::array<uint32, 4> ARENA_EMOTES = {TEXT_EMOTE_ROAR, TEXT_EMOTE_FLEX, TEXT_EMOTE_CHEER, TEXT_EMOTE_APPLAUD};

template <std::size_t N>
uint32 Pick(std::array<uint32, N> const& emotes)
{
    return emotes[urand(0, uint32(N) - 1)];
}

// The style's emote list; 0 for a style without one.
uint32 PickEmote(uint8 style)
{
    switch (style)
    {
        case ACTIVITY_STYLE_INN:
            return Pick(INN_EMOTES);
        case ACTIVITY_STYLE_DANCE:
            return Pick(DANCE_EMOTES);
        case ACTIVITY_STYLE_LOITER:
            return Pick(LOITER_EMOTES);
        case ACTIVITY_STYLE_ARENA:
            return Pick(ARENA_EMOTES);
        default:
            return 0;
    }
}

// `face` on the bot's map and close enough to see a text emote.
bool InEmoteRange(Player* bot, Player* face)
{
    return face && face != bot && face->IsInWorld() &&
           bot->GetExactDist(face) <= sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE);
}

// A text emote at `face` as the "coa duel emote" action plays it: faces it unless moving.
void EmoteAt(Player* bot, Player* face, uint32 emote)
{
    if (!bot->isMoving() && !bot->HasInArc(static_cast<float>(M_PI), face))
        ServerFacade::instance().SetFacingTo(bot, face);

    WorldPacket data(SMSG_TEXT_EMOTE);
    data << uint32(emote);
    data << EmoteActionBase::GetNumberOfEmoteVariants(TextEmotes(emote), bot->getRace(), bot->getGender());
    data << face->GetGUID();
    bot->GetSession()->HandleTextEmoteOpcode(data);
}
}  // namespace

bool CoaPartyEmoteAction::Execute(Event /*event*/)
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    ActivityMemberView const* r = s.row;
    uint64 const now = CoaTime::NowMs();
    // A dance lasts until the bot moves: the next emote waits longer.
    bool const dance = r && r->style == ACTIVITY_STYLE_DANCE;
    s.nextPartyEmoteMs = now + (dance ? urand(40000, 70000) : urand(15000, 40000));
    if (!r)
        return false;

    // At the inn, one time in four the bot sits down instead (it stands up again as soon as it moves).
    if (r->style == ACTIVITY_STYLE_INN && !urand(0, 3) && bot->getStandState() == UNIT_STAND_STATE_STAND)
    {
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }

    uint32 const emote = PickEmote(r->style);
    if (!emote)
        return false;

    // Toward someone of the same activity, or a real player, close by.
    std::vector<Player*> faces;
    GuidVector const nearby = AI_VALUE(GuidVector, "nearest friendly players");
    for (ObjectGuid const& guid : nearby)
    {
        Player* p = ObjectAccessor::GetPlayer(*bot, guid);
        if (!p || p == bot || bot->GetExactDist(p) > PARTY_FACE_YARDS || !InEmoteRange(bot, p))
            continue;
        ActivityMemberView const* other = s.snapshot ? s.snapshot->Find(p->GetGUID().GetCounter()) : nullptr;
        if ((other && other->activityId == r->activityId) || CoaActivityBot::HasRealSession(p))
            faces.push_back(p);
    }

    if (faces.empty())
        botAI->PlayEmote(emote);
    else
        EmoteAt(bot, faces[urand(0, uint32(faces.size()) - 1)], emote);
    return true;
}

bool CoaPartyGreetAction::isUseful()
{
    return !AI_VALUE(ActivityBotState&, "coa activity state").greetTarget.IsEmpty();
}

bool CoaPartyGreetAction::Execute(Event /*event*/)
{
    ActivityBotState& s = AI_VALUE(ActivityBotState&, "coa activity state");
    ObjectGuid const guid = s.greetTarget;
    s.greetTarget.Clear();

    Player* p = guid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, guid);
    if (!InEmoteRange(bot, p))
        return false;

    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    if (!bot->isMoving())
        ServerFacade::instance().SetFacingTo(bot, p);
    EmoteAt(bot, p, urand(0, 1) ? TEXT_EMOTE_WAVE : TEXT_EMOTE_HELLO);
    s.greetedMs[guid.GetCounter()] = CoaTime::NowMs();
    return true;
}

bool CoaPartyMountAction::Execute(Event /*event*/)
{
    ActivityBotState& s = AI_VALUE(ActivityBotState&, "coa activity state");
    s.nextMountMs = CoaTime::NowMs() + MOUNT_RETRY_MS;

    // The bot's own "check mount state" (its preferred or best mount). Its isUseful sets the master and forms Mount
    // reads, and says no where a mount is not allowed (indoors, too low, no "mount" strategy).
    CheckMountStateAction* check = dynamic_cast<CheckMountStateAction*>(context->GetAction("check mount state"));
    return check && check->isUseful() && check->Mount();
}
