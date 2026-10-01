/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Escort.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "CoaThread.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "GridDefines.h"
#include "LFGMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr uint64 LINE_RETRY_MS = 15000;
constexpr float SPOT_HEIGHT_TOLERANCE = 20.0f;
constexpr uint32 CANDIDATE_POOL = 32;

struct EscortState : ActivityKindState
{
    ActivityPoint destination;
    uint32 expectedBots = 0;
    uint64 formUntilMs = 0;
    uint64 inviteUntilMs = 0;
    uint64 followUntilMs = 0;
    bool invited = false;
    bool accepted = false;
    bool leadHanded = false;
    std::vector<ObjectGuid> toAdd;
    std::string lineDue;
    uint64 lineUntilMs = 0;
};

EscortState* StateOf(Activity& activity) { return dynamic_cast<EscortState*>(activity.state.get()); }

Player* Requester(Activity const& activity)
{
    return activity.requester.IsEmpty() ? nullptr : ObjectAccessor::FindConnectedPlayer(activity.requester);
}

Group* EscortGroup(Activity const& activity)
{
    return activity.groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
}

// These rules keep the ordinary group AI in PvE. Map-thread target guards also apply between director ticks.
char const* Unsafe(Player* player, uint32 mapId)
{
    uint32 hazards = 0;
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || !player->GetSession() ||
        player->GetSession()->isLogingOut())
        return EscortRules::WhyUnfit(EscortRules::Offline, mapId, mapId);
    if (!player->IsAlive())
        hazards |= EscortRules::Dead;
    if (!player->GetMap() || player->GetMap()->Instanceable())
        hazards |= EscortRules::Instance;
    if (player->GetVehicle())
        return "in a vehicle";
    if (player->IsInFlight())
        hazards |= EscortRules::Flight;
    if (player->IsBeingTeleported())
        hazards |= EscortRules::Teleport;
    if (player->IsPvP())
        hazards |= EscortRules::Pvp;
    if (player->IsFFAPvP())
        hazards |= EscortRules::Ffa;
    if (player->HasAura(ActivityThreshold::SPELL_WAR_MODE))
        hazards |= EscortRules::WarMode;
    if (player->HasAura(ActivityThreshold::SPELL_HIGH_RISK))
        hazards |= EscortRules::HighRisk;
    if (player->HasAura(ActivityThreshold::SPELL_MERCENARY))
        hazards |= EscortRules::Mercenary;
    if (player->duel)
        hazards |= EscortRules::Duel;
    if (player->InBattleground() || player->InArena() || player->InBattlegroundQueue())
        hazards |= EscortRules::Battleground;
    if (sLFGMgr->GetState(player->GetGUID()) != lfg::LFG_STATE_NONE)
        hazards |= EscortRules::Lfg;
    return EscortRules::WhyUnfit(hazards, player->GetMapId(), mapId);
}

char const* RequesterUnfit(Player* player, uint32 mapId, ObjectGuid allowedGroup = ObjectGuid::Empty)
{
    if (char const* const why = Unsafe(player, mapId))
        return why;
    if (player->GetSession()->IsBot())
        return "not a real player";
    if (player->isAFK() || player->isDND())
        return "AFK or DND";
    if (player->GetGroup() && (allowedGroup.IsEmpty() || player->GetGroup()->GetGUID() != allowedGroup))
        return "in another group";
    return nullptr;
}

bool SpotPoint(ActivitySpot const& spot, ActivityPoint& point)
{
    MapEntry const* const map = sMapStore.LookupEntry(spot.mapId);
    if (!map || map->Instanceable() || !Acore::IsValidMapCoord(spot.x, spot.y, spot.z))
        return false;
    point.mapId = spot.mapId;
    point.x = spot.x;
    point.y = spot.y;
    point.z = spot.z;
    point.set = true;
    ActivityPoint const ground = ActivityWorld::GroundNear(point, 0.0f);
    if (!ground.set || std::abs(ground.z - spot.z) > SPOT_HEIGHT_TOLERANCE)
        return false;
    point = ground;
    return true;
}

bool NearbySafeBot(Player* bot, Player* requester, ActivityCandidate const& candidate, ActivityConfig const& config,
                   uint8 minLevel, uint8 maxLevel)
{
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    return requester && ai && ai->GetCoaEpoch() == candidate.epoch &&
           !Unsafe(bot, requester->GetMapId()) && bot->GetMap() == requester->GetMap() &&
           bot->InSamePhase(requester) &&
           EscortRules::Compatible(bot->GetTeamId(), requester->GetTeamId(), bot->GetLevel(),
                                   requester->GetLevel(), config.escortLevelSpread) &&
           bot->GetLevel() >= minLevel && bot->GetLevel() <= maxLevel &&
           bot->GetExactDist(requester) <= config.escortNearbyYards;
}

void Line(ActivityServices& services, Activity& activity, EscortState& state)
{
    if (state.lineDue.empty())
        return;
    if (services.nowMs >= state.lineUntilMs)
    {
        state.lineDue.clear();
        return;
    }
    ActivityOpResult const result = services.ops.PostToChannel(activity.leader, ActivityChatTarget::Whisper,
                                                              state.lineDue, activity.requester);
    if (result != ActivityOpResult::Deferred && result != ActivityOpResult::Refused)
        state.lineDue.clear();
}

void QueueLine(ActivityServices& services, EscortState& state, char const* key, char const* english,
               std::string const& destination)
{
    state.lineDue = ActivityWorld::Line(key, english, {{"%target", destination}});
    state.lineUntilMs = services.nowMs + LINE_RETRY_MS;
}

bool BotsReady(Activity& activity, EscortState const& state, Player* requester, ActivityConfig const& config)
{
    if (activity.BotsIn() != state.expectedBots)
        return false;
    Group* const group = EscortGroup(activity);
    if (!activity.groupGuid.IsEmpty() && (!group || group->isRaidGroup()))
        return false;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.isBot || !member.In())
            continue;
        Player* const bot = ActivityWorld::FindMember(member);
        if (Unsafe(bot, state.destination.mapId) || bot->GetMap() != requester->GetMap() ||
            !bot->InSamePhase(requester) ||
            !EscortRules::Compatible(bot->GetTeamId(), requester->GetTeamId(), bot->GetLevel(),
                                     requester->GetLevel(), config.escortLevelSpread))
            return false;
        if (!group)
        {
            if (bot->GetGroup() || GET_PLAYERBOT_AI(bot)->GetMaster())
                return false;
        }
        else if (bot->GetGroup() != group)
        {
            bool const waiting = std::find(state.toAdd.begin(), state.toAdd.end(), member.guid) != state.toAdd.end();
            if (bot->GetGroup() || !waiting)
                return false;
        }
    }
    // A requester may invite friends after taking leadership. End our help and let them keep their group.
    if (group && group->GetMembersCount() > state.expectedBots + (state.accepted ? 1 : 0))
        return false;
    return true;
}

void Form(ActivityServices& services, Activity& activity, EscortState& state)
{
    if (services.nowMs >= state.formUntilMs)
    {
        activity.Finish(ActivityOutcome::TooFew, "escort group could not be formed in time");
        return;
    }
    if (activity.groupGuid.IsEmpty())
    {
        std::vector<ObjectGuid> members;
        for (ActivityMember const& member : activity.members)
            if (member.isBot && member.In() && member.guid != activity.leader)
                members.push_back(member.guid);
        // Use one whole-budget tick where possible; a smaller configured budget still adds members over ticks.
        uint32 const cost = 1 + uint32(members.size());
        if (cost <= services.config.maxOpsPerTick && services.ops.Used() + cost > services.config.maxOpsPerTick)
            return;
        std::vector<ObjectGuid> added;
        std::vector<ObjectGuid> skipped;
        ObjectGuid groupGuid;
        ActivityOpResult const result = services.ops.FormGroup(activity.leader, members, uint8(GROUP_LOOT),
                                                               uint8(ITEM_QUALITY_UNCOMMON), groupGuid, added, skipped);
        if (result == ActivityOpResult::Deferred)
            return;
        if (result != ActivityOpResult::Done || groupGuid.IsEmpty())
        {
            activity.Finish(ActivityOutcome::TooFew, "escort group could not be formed");
            return;
        }
        activity.groupGuid = groupGuid;
        activity.viewDirty = true;
        state.toAdd = std::move(skipped);
    }
    for (auto it = state.toAdd.begin(); it != state.toAdd.end();)
    {
        ActivityOpResult const result = services.ops.AddMember(activity.groupGuid, activity.leader, *it);
        if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
            return;
        if (result != ActivityOpResult::Done)
        {
            activity.Finish(ActivityOutcome::TooFew, "an escort bot could not join the group");
            return;
        }
        it = state.toAdd.erase(it);
    }
    Group* const group = EscortGroup(activity);
    if (!group || group->GetMembersCount() != state.expectedBots)
    {
        activity.Finish(ActivityOutcome::TooFew, "the escort group is incomplete");
        return;
    }
    state.inviteUntilMs = services.nowMs + uint64(services.config.escortInviteSeconds) * 1000;
    activity.SetPhase(ActivityPhase::Recruiting, services.nowMs);
}

void Invite(ActivityServices& services, Activity& activity, EscortState& state, Player* requester)
{
    if (state.accepted)
        return;
    if (services.nowMs >= state.inviteUntilMs)
    {
        activity.Finish(ActivityOutcome::Timeout, "escort invitation expired");
        return;
    }
    if (state.invited)
    {
        Group* const group = EscortGroup(activity);
        if (!group || requester->GetGroupInvite() != group)
            activity.Finish(ActivityOutcome::Stopped, "escort invitation declined or cancelled");
        return;
    }
    ActivityOpResult const result = services.ops.InvitePlayer(activity.leader, activity.requester);
    if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
        return;
    if (result != ActivityOpResult::Done)
    {
        activity.Finish(ActivityOutcome::Stopped, "escort invitation could not be sent");
        return;
    }
    state.invited = true;
    state.inviteUntilMs = services.nowMs + uint64(services.config.escortInviteSeconds) * 1000;
    activity.invites[activity.requester] = state.inviteUntilMs;
    QueueLine(services, state, "coa_act_escort_invite", "Accept my invitation and we will walk with you to %target.",
              activity.name);
}

void Follow(ActivityServices& services, Activity& activity, EscortState& state, Player* requester)
{
    ActivityMember const* const member = activity.FindMember(activity.requester);
    Group* const group = EscortGroup(activity);
    if (!member || !member->In() || member->isBot || !group || requester->GetGroup() != group)
    {
        activity.Finish(ActivityOutcome::Stopped, "the requester left the escort group");
        return;
    }
    if (!state.leadHanded)
    {
        ActivityOpResult const result = services.ops.HandLeader(activity.groupGuid, activity.requester);
        if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
            return;
        if (result != ActivityOpResult::Done)
        {
            activity.Finish(ActivityOutcome::Stopped, "escort leadership could not be handed to the requester");
            return;
        }
        state.leadHanded = true;
    }
    activity.SetFollow(activity.requester);
    if (services.nowMs >= state.followUntilMs)
    {
        activity.Finish(ActivityOutcome::Timeout, "escort time limit reached");
        return;
    }
    if (EscortRules::Arrived(state.destination, requester->GetMapId(), requester->GetPositionX(),
                              requester->GetPositionY(), requester->GetPositionZ(), services.config.escortArrivalYards))
    {
        // Completion never moves the requester, grants rewards, or delays the director's ordinary cleanup.
        services.ops.PostToChannel(activity.leader, ActivityChatTarget::Whisper,
                                   ActivityWorld::Line("coa_act_escort_arrived", "Here we are. Safe travels!"),
                                   activity.requester);
        activity.Finish(ActivityOutcome::Success, "requester reached the escort destination");
    }
}
}  // namespace

bool EscortKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                      std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    ActivityConfig const& config = services.config;
    if (!config.escortEnable)
    {
        why = "escorts are disabled";
        return false;
    }
    ActivitySpot const* const spot = config.FindEscortSpot(request.target);
    Player* const requester = request.nearPlayer;
    if (!spot)
    {
        why = "unknown escort destination (Escort.Spots)";
        return false;
    }
    if (char const* const unfit = RequesterUnfit(requester, spot->mapId))
    {
        why = Acore::StringFormat("requester {}", unfit);
        return false;
    }
    if (requester->GetGroupInvite())
    {
        why = "requester already has a group invitation";
        return false;
    }
    uint8 const team = uint8(requester->GetTeamId());
    if ((spot->team != TEAM_NEUTRAL && spot->team != team) || (request.team && *request.team != team))
    {
        why = "escort destination is for the other faction";
        return false;
    }
    ActivityPoint destination;
    if (!SpotPoint(*spot, destination))
    {
        why = "escort destination is not on dry overworld ground";
        return false;
    }
    if (!request.capacity)
    {
        why = "no participant places available";
        return false;
    }
    int32 const level = int32(requester->GetLevel());
    uint8 const minLevel = uint8(std::max<int32>(1, level - int32(config.escortLevelSpread)));
    uint8 const maxLevel = uint8(std::min<int32>(80, level + int32(config.escortLevelSpread)));
    ActivityCandidateQuery query;
    query.team = team;
    query.minLevel = minLevel;
    query.maxLevel = maxLevel;
    query.center = ActivityWorld::PointOf(requester);
    query.sameMapOnly = true;
    query.maxDistance = config.escortNearbyYards;
    query.rulesetSafe = true;
    query.limit = CANDIDATE_POOL;
    std::vector<ActivityCandidate> candidates;
    for (ActivityCandidate const& candidate : services.candidates.Find(query))
    {
        Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
        if (NearbySafeBot(bot, requester, candidate, config, minLevel, maxLevel))
            candidates.push_back(candidate);
    }
    if (candidates.empty())
    {
        why = "no nearby free PvE bots in the requester's level band";
        return false;
    }
    uint32 const wanted = std::min({std::clamp(config.escortBots, uint32(1), uint32(4)), request.capacity,
                                   uint32(candidates.size())});
    candidates.resize(std::min<std::size_t>(candidates.size(), 2 * wanted));
    plan.kind = Id();
    plan.gm = false;  // A GM client requests the same player-safe escort; there is no console bypass.
    plan.team = team;
    plan.requester = requester->GetGUID();
    plan.minLevel = minLevel;
    plan.maxLevel = maxLevel;
    plan.rally = query.center;
    plan.target = spot->name;
    plan.name = spot->name;
    plan.wanted = wanted;
    plan.needed = 1;
    plan.leaderFixed = true;
    plan.candidates = std::move(candidates);
    plan.decisions.clear();
    auto state = std::make_unique<EscortState>();
    state->destination = destination;
    plan.state = std::move(state);
    return true;
}

bool EscortKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    EscortState* const state = dynamic_cast<EscortState*>(plan.state.get());
    Player* const requester = plan.requester.IsEmpty() ? nullptr : ObjectAccessor::FindConnectedPlayer(plan.requester);
    if (!services.config.escortEnable || !state || plan.candidates.empty() || plan.decisions.empty() ||
        plan.decisions[0] != 1 || RequesterUnfit(requester, state->destination.mapId) || requester->GetGroupInvite())
        return false;
    std::vector<ActivityCandidate const*> accepted;
    for (std::size_t i = 0; i < plan.candidates.size() && accepted.size() < plan.wanted; ++i)
    {
        if (i >= plan.decisions.size() || plan.decisions[i] != 1)
            continue;
        ActivityCandidate const& candidate = plan.candidates[i];
        Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
        if (!ActivityCandidates::IsFree(bot) || bot->IsInCombat() ||
            !NearbySafeBot(bot, requester, candidate, services.config, plan.minLevel, plan.maxLevel))
        {
            if (!i)
                return false;
            continue;
        }
        accepted.push_back(&candidate);
    }
    if (accepted.empty())
        return false;
    activity.requester = plan.requester;
    activity.leader = accepted.front()->guid;
    activity.name = plan.name;
    activity.target = plan.target;
    activity.minLevel = plan.minLevel;
    activity.maxLevel = plan.maxLevel;
    for (ActivityCandidate const* const candidate : accepted)
        activity.Enroll(candidate->guid, candidate->epoch, true, candidate->level, services.nowUnix);
    state->expectedBots = uint32(accepted.size());
    state->formUntilMs = services.nowMs + ActivityThreshold::FORM_TIMEOUT_MS;
    activity.hardDeadlineMs = state->formUntilMs + uint64(2 * services.config.escortInviteSeconds) * 1000 +
                              uint64(services.config.escortMinutes) * 60 * 1000;
    activity.SetCenter(state->destination, services.config.escortArrivalYards);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    activity.SetPhase(ActivityPhase::Forming, services.nowMs);
    activity.state = std::move(plan.state);
    return true;
}

void EscortKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;
    EscortState* const state = StateOf(activity);
    if (!state)
    {
        activity.Finish(ActivityOutcome::Stopped, "escort state missing");
        return;
    }
    if (!services.config.escortEnable)
    {
        activity.Finish(ActivityOutcome::Disabled, "escorts disabled");
        return;
    }
    Player* const requester = Requester(activity);
    if (char const* const unfit = RequesterUnfit(requester, state->destination.mapId, activity.groupGuid))
    {
        activity.Finish(ActivityOutcome::Stopped, Acore::StringFormat("requester {}", unfit));
        return;
    }
    if (!BotsReady(activity, *state, requester, services.config))
    {
        activity.Finish(ActivityOutcome::TooFew, "an escort bot or group is no longer available for PvE");
        return;
    }
    // Enforce walking even if a future shared director phase changes a field.
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    if (activity.phase == ActivityPhase::Forming)
        Form(services, activity, *state);
    if (activity.phase == ActivityPhase::Recruiting && !activity.finishing)
        Invite(services, activity, *state, requester);
    if (activity.phase == ActivityPhase::Active && !activity.finishing)
        Follow(services, activity, *state, requester);
    if (!activity.finishing)
        Line(services, activity, *state);
}

void EscortKind::OnMemberLeft(ActivityServices& /*services*/, Activity& activity, ActivityMember const& member)
{
    COA_ASSERT_WORLD_THREAD();
    if (member.guid == activity.requester)
        activity.Finish(ActivityOutcome::Stopped, "the requester left the escort");
    else if (member.isBot)
        activity.Finish(ActivityOutcome::TooFew, "an escort bot left");
}

std::string EscortKind::Join(ActivityServices& services, Activity& activity, Player* player)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Accepts(services, activity, player))
        return ActivityWorld::Line("coa_act_escort_private", "That escort is reserved for the player who requested it.");
    EscortState* const state = StateOf(activity);
    if (!state)
        return ActivityWorld::Line("coa_act_join_none", "No bot activity you can join right now.");
    Invite(services, activity, *state, player);
    return ActivityWorld::Line("coa_act_escort_pending", "Your escort invitation is pending. Accept it to start the walk.");
}

bool EscortKind::Accepts(ActivityServices& /*services*/, Activity const& activity, Player* player)
{
    EscortState const* const state = dynamic_cast<EscortState const*>(activity.state.get());
    return player && state && player->GetGUID() == activity.requester && !activity.finishing &&
           activity.phase == ActivityPhase::Recruiting && !state->accepted &&
           !RequesterUnfit(player, state->destination.mapId) && !activity.groupGuid.IsEmpty();
}

bool EscortKind::OnPlayerJoined(ActivityServices& services, Activity& activity, ActivityMember const& member)
{
    COA_ASSERT_WORLD_THREAD();
    EscortState* const state = StateOf(activity);
    Player* const requester = Requester(activity);
    Group* const group = EscortGroup(activity);
    if (!services.config.escortEnable || !state || member.isBot || member.guid != activity.requester || !group ||
        activity.phase != ActivityPhase::Recruiting ||
        !EscortRules::CanAcceptInvitation(state->invited, state->accepted, services.nowMs, state->inviteUntilMs) ||
        RequesterUnfit(requester, state->destination.mapId, activity.groupGuid) || requester->GetGroup() != group)
    {
        activity.Finish(ActivityOutcome::Stopped, "escort acceptance is no longer valid");
        return true;
    }
    state->accepted = true;
    state->followUntilMs = services.nowMs + uint64(services.config.escortMinutes) * 60 * 1000;
    activity.hardDeadlineMs = state->followUntilMs;
    activity.SetFollow(activity.requester);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
    QueueLine(services, *state, "coa_act_escort_ready", "We are with you. Lead the way to %target.", activity.name);
    return true;  // The queued welcome replaces the director's generic meeting-place whisper.
}

std::string EscortKind::Describe(Activity const& activity, uint64 nowMs) const
{
    EscortState const* const state = dynamic_cast<EscortState const*>(activity.state.get());
    if (!state)
        return "escort state missing";
    uint64 const deadline = state->accepted ? state->followUntilMs : state->invited ? state->inviteUntilMs
                                                                                 : state->formUntilMs;
    return Acore::StringFormat("requester {}, destination {} ({:.1f}, {:.1f}, {:.1f}), {}, {} s left",
                               activity.requester.GetCounter(), state->destination.mapId, state->destination.x,
                               state->destination.y, state->destination.z, state->accepted ? "following" : "inviting",
                               deadline > nowMs ? (deadline - nowMs) / 1000 : 0);
}

std::unique_ptr<ActivityKind> CreateEscortKind() { return std::make_unique<EscortKind>(); }
