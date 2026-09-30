/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaActivityActions.h"
#include "ActivityConfig.h"
#include "ActivityDuelCooldowns.h"
#include "ActivityRegistry.h"
#include "BrainTypes.h"
#include "ChatHelper.h"
#include "CoaActivityValues.h"
#include "CoaPlayerPrefs.h"
#include "CoaTime.h"
#include "EmoteAction.h"
#include "Event.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr float DUEL_CAST_YARDS = 9.0f;      // the Duel spell (7266) reaches 10 yards
constexpr float DUEL_APPROACH_STOP = 6.0f;   // the walk to a partner stops this far from it (the trigger's ring reach)
constexpr float RING_EMOTE_YARDS = 20.0f;

constexpr std::array<uint32, 6> CELEBRATE_EMOTES = {TEXT_EMOTE_CHEER, TEXT_EMOTE_ROAR,    TEXT_EMOTE_FLEX,
                                                    TEXT_EMOTE_DANCE, TEXT_EMOTE_VICTORY, TEXT_EMOTE_APPLAUD};
constexpr std::array<uint32, 5> RING_IDLE_EMOTES = {TEXT_EMOTE_FLEX, TEXT_EMOTE_ROAR, TEXT_EMOTE_CHEER,
                                                    TEXT_EMOTE_SALUTE, TEXT_EMOTE_TRAIN};

// Plays a text emote at `face` the way EmoteAction does (the voice director's helper): faces it unless moving.
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

// A real target's rules of the partner scan (contract 12.3) that can change while the bot walks up to it: AFK or
// DND, GM mode, the duel opt-out, the account's daily duels.
bool RealTargetStillAllowed(Player* target, uint32 account, ActivityConfig const& config)
{
    if (target->isAFK() || target->isDND() || target->IsGameMaster())
        return false;

    CoaPlayerPrefs const& prefs = CoaPlayerPrefs::Instance();
    if (!prefs.IsLoaded() || (prefs.Get(account).activityMask & COA_OPTOUT_DUELS))
        return false;

    return !config.duelMaxAcceptsPerPlayerPerDay ||
           ActivityDuelCooldowns::Instance().RealDuelsToday(account, BrainClock::UtcDayNow()) <
               config.duelMaxAcceptsPerPlayerPerDay;
}

// `face` on the bot's map and close enough to see a text emote.
bool InEmoteRange(Player* bot, Player* face)
{
    return face && face != bot && face->IsInWorld() &&
           bot->GetExactDist(face) <= sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE);
}
}  // namespace

// --- "coa activity" ---

bool CoaActivityTravelAction::isUseful()
{
    return CoaActivityBot::Refresh(botAI).row && !bot->IsInCombat() && !bot->IsInFlight();
}

bool CoaActivityTravelAction::Execute(Event /*event*/)
{
    ActivityMemberView const* r = CoaActivityBot::Refresh(botAI).row;
    if (!r)
        return false;

    if (r->phase == ActivityPhase::Gathering)
        return ActivityMoveFarTo(StableJitter(r->center, ActivityThreshold::RALLY_ARRIVE_RADIUS, r->phase));

    // The leader walks the route; its members follow it.
    if (r->phase == ActivityPhase::Marching)
        return ActivityMoveFarTo(r->center);

    return false;
}

bool CoaActivityHoldAction::Execute(Event /*event*/)
{
    ForceToWait(3000);
    return true;
}

bool CoaActivityReturnAction::Execute(Event /*event*/)
{
    ActivityMemberView const* r = CoaActivityBot::Refresh(botAI).row;
    if (!r)
        return false;

    return ActivityMoveFarTo(StableJitter(r->center, r->radius / 2.0f, r->phase));
}

bool CoaActivityCatchUpAction::Execute(Event /*event*/)
{
    ActivityMemberView const* r = CoaActivityBot::Refresh(botAI).row;
    Player* leader = r ? CoaActivityBot::LeaderOnMap(bot, *r) : nullptr;
    if (!leader)
        return false;

    // No jitter: "follow" takes over once close.
    return ActivityMoveFarTo(CoaActivityBot::PointOf(leader));
}

bool CoaActivityCelebrateAction::Execute(Event /*event*/)
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    ActivityMemberView const* r = s.row;
    if (!r)
        return false;

    // Toward the leader, or the ring's center.
    if (!bot->isMoving())
    {
        if (Player* leader = CoaActivityBot::LeaderOnMap(bot, *r))
            ServerFacade::instance().SetFacingTo(bot, leader);
        else if (r->center.IsSet() && r->center.mapId == bot->GetMapId() &&
                 bot->GetExactDist2d(r->center.x, r->center.y) > 2.0f)
            bot->SetFacingTo(bot->GetAngle(r->center.x, r->center.y));
    }

    uint32 const emote = CELEBRATE_EMOTES[(bot->GetGUID().GetCounter() + r->activityId) % CELEBRATE_EMOTES.size()];
    botAI->PlayEmote(emote);
    s.celebratedActivity = r->activityId;
    return true;
}

bool CoaActivityRoamAction::Execute(Event /*event*/)
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    s.nextRoamMs = CoaTime::NowMs() + 20000;

    ActivityMemberView const* r = s.row;
    if (!r)
        return false;

    // 0.18..0.6 of the leash from the center, so "grind" finds something new.
    return ActivityMoveFarTo(StableJitter(r->center, r->radius * 0.6f, r->phase));
}

// --- "coa duel" ---

bool CoaChallengeDuelAction::isUseful()
{
    return !AI_VALUE(ActivityBotState&, "coa activity state").duelTarget.IsEmpty() && !bot->duel;
}

bool CoaChallengeDuelAction::Execute(Event /*event*/)
{
    ActivityBotState& s = AI_VALUE(ActivityBotState&, "coa activity state");
    ObjectGuid const targetGuid = s.duelTarget;

    // The trigger's per-target rules again: another bot may have taken it, or it walked off.
    Player* target = ObjectAccessor::GetPlayer(*bot, targetGuid);
    if (!target || target == bot || !target->IsAlive() || target->duel || target->IsInCombat() ||
        bot->GetExactDist2d(target) > 30.0f)
    {
        s.duelTarget.Clear();
        return false;
    }

    // The rules that can change during the walk (up to DUEL_APPROACH_MS): a player invited the bot or became its
    // master, or a real target went AFK/DND, opted out or used up its daily duels meanwhile.
    std::shared_ptr<ActivityConfig const> const config = ActivityConfig::Current();
    uint32 const account = CoaActivityBot::HasRealSession(target) ? target->GetSession()->GetAccountId() : 0;
    if (bot->GetGroup() || botAI->HasGameClientMaster() ||
        (account && !RealTargetStillAllowed(target, account, *config)))
    {
        s.duelTarget.Clear();
        s.duelApproachUntilMs = 0;
        return false;
    }

    // Out of the spell's reach: walk up to it, and keep the target (the trigger fires until duelApproachUntilMs).
    if (bot->GetExactDist2d(target) > DUEL_CAST_YARDS)
        return MoveTo(target, DUEL_APPROACH_STOP);

    s.duelTarget.Clear();
    s.duelApproachUntilMs = 0;

    uint64 const now = CoaTime::NowMs();
    if (!ActivityDuelCooldowns::Instance().TryStampChallenge(
            bot->GetGUID().GetCounter(), target->GetGUID().GetCounter(), account, now,
            config->duelPairCooldownMinutes * 60000, config->duelPlayerCooldownMinutes * 60000))
        return false;

    // Face it first, or "cast custom spell" turns and defers the cast through a command.
    if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
        ServerFacade::instance().SetFacingTo(bot, target);

    if (!botAI->DoSpecificAction(
            "cast custom spell",
            Event("coa challenge duel", chat->FormatWorldobject(target) + " " +
                                            std::to_string(ActivityThreshold::DUEL_SPELL)),
            true))
        return false;

    EmoteAt(bot, target, TEXT_EMOTE_BOW);
    s.challengeSentMs = now;
    LOG_DEBUG("playerbots.activity", "{} challenges {} to a duel", bot->GetName(), target->GetName());
    return true;
}

bool CoaDuelCancelAction::Execute(Event /*event*/)
{
    ActivityBotState& s = AI_VALUE(ActivityBotState&, "coa activity state");
    s.challengeSentMs = 0;

    if (!bot->duel || bot->duel->Initiator != bot || bot->duel->State != DUEL_STATE_CHALLENGED)
        return false;

    // The initiator withdraws: the duel-end hook does not count it as a decline.
    WorldPacket packet(CMSG_DUEL_CANCELLED, 8);
    packet << bot->GetGuidValue(PLAYER_DUEL_ARBITER);
    bot->GetSession()->HandleDuelCancelledOpcode(packet);
    LOG_DEBUG("playerbots.activity", "{} withdraws an unanswered duel challenge", bot->GetName());
    return true;
}

bool CoaDuelEmoteAction::isUseful() { return AI_VALUE(ActivityBotState&, "coa activity state").pendingEmote != 0; }

bool CoaDuelEmoteAction::Execute(Event /*event*/)
{
    ActivityBotState& s = AI_VALUE(ActivityBotState&, "coa activity state");
    uint32 const emote = s.pendingEmote;
    ObjectGuid const faceGuid = s.emoteFace;
    s.pendingEmote = 0;
    s.emoteFace.Clear();
    if (!emote)
        return false;

    Player* face = faceGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, faceGuid);
    if (InEmoteRange(bot, face))
        EmoteAt(bot, face, emote);
    else
        botAI->PlayEmote(emote);
    return true;
}

bool CoaRingIdleAction::Execute(Event /*event*/)
{
    ActivityBotState& s = CoaActivityBot::Refresh(botAI);
    s.nextIdleMs = CoaTime::NowMs() + urand(10000, 20000);

    ActivityMemberView const* r = s.row;
    if (!r)
        return false;

    // Two times out of three a short walk around the spot.
    if (urand(0, 2))
    {
        ActivityMoveFarTo(StableJitter(r->center, ActivityThreshold::RING_IDLE_RADIUS, r->phase));
        return true;
    }

    // Else a friendly emote at a ring partner in sight.
    std::vector<Player*> partners;
    GuidVector const nearby = AI_VALUE(GuidVector, "nearest friendly players");
    for (ObjectGuid const& guid : nearby)
    {
        Player* p = ObjectAccessor::GetPlayer(*bot, guid);
        if (!p || p == bot || bot->GetExactDist(p) > RING_EMOTE_YARDS || !InEmoteRange(bot, p))
            continue;
        ActivityMemberView const* other = s.snapshot ? s.snapshot->Find(p->GetGUID().GetCounter()) : nullptr;
        if (other && other->activityId == r->activityId)
            partners.push_back(p);
    }

    if (!partners.empty())
        EmoteAt(bot, partners[urand(0, uint32(partners.size()) - 1)],
                RING_IDLE_EMOTES[urand(0, uint32(RING_IDLE_EMOTES.size()) - 1)]);
    return true;
}

// --- "coa activity combat" ---

bool CoaActivityAttackObjectiveAction::Execute(Event /*event*/)
{
    ActivityMemberView const* r = CoaActivityBot::Refresh(botAI).row;
    Unit* objective = r ? CoaActivityBot::ObjectiveInReach(botAI, *r) : nullptr;
    if (!objective)
        return false;

    context->GetValue<GuidVector>("prioritized targets")->Set({objective->GetGUID()});
    return Attack(objective);
}
