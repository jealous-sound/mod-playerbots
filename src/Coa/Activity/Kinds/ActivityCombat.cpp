/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#include "ActivityCombat.h"
#include "ActivityConfig.h"
#include "ActivityGuards.h"
#include "ActivityOperations.h"
#include "ActivityPvePolicy.h"
#include "ActivityPvpPolicy.h"
#include "CoaThread.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "Escort.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "RareScout.h"
#include "PlayerbotAIConfig.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>

namespace
{
struct CombatState : ActivityKindState
{
    ActivitySpawnInfo spawn;
    ActivityPoint anchor;
    ObjectGuid target;
    uint32 ruleset = 0;
    uint64 nextScanMs = 0;
    uint64 endMs = 0;
    uint64 gatherUntilMs = 0;
    uint32 waypoint = 0;
    bool engaged = false;
    bool grouped = false;
    std::vector<ObjectGuid> toAdd;
    uint32 minimum = 0;
    bool patrolObserved = false;
    ActivityOutcome retireOutcome = ActivityOutcome::None;
    std::string retireSummary;
    ActivityPoint escape;
    float escapeDistance = 0.0f;
    uint64 retireUntilMs = 0;
};

bool CurrentBotSafe(Activity const& activity, ActivityMember const& member, Player* bot, Player* leader)
{
    return member.isBot && bot && bot->GetSession() && bot->GetSession()->IsBot() && bot->IsAlive() &&
        bot->IsInWorld() && bot->GetMap() && !bot->GetMap()->Instanceable() &&
        bot->GetMap() == leader->GetMap() && bot->InSamePhase(leader) &&
        bot->GetTeamId() == activity.team && bot->GetLevel() >= activity.minLevel &&
        bot->GetLevel() <= activity.maxLevel && !bot->IsInFlight() && !bot->GetVehicle() &&
        !bot->IsBeingTeleported() && !bot->duel && !bot->InBattleground() && !bot->InArena() &&
        !bot->InBattlegroundQueue() && !bot->HasAura(ActivityThreshold::SPELL_MERCENARY);
}

bool RulesetReady(Player* bot, ActivityMember const& member, uint32 ruleset)
{
    return !ruleset || (member.rulesetApplied && bot->IsPvP() && bot->HasAura(ruleset) &&
        !bot->HasAura(ActivityThreshold::SPELL_PVE));
}

bool HumansNear(ActivityServices& services, ActivityPoint const& point, float radius)
{
    return ActivityWorld::RealPlayerNear(services.realPlayers, point, radius);
}

bool RareTemplate(ActivitySpawnInfo const& spawn)
{
    CreatureData const* const data = sObjectMgr->GetCreatureData(spawn.spawnId);
    CreatureTemplate const* const info = data ? sObjectMgr->GetCreatureTemplate(data->id) : nullptr;
    MapEntry const* const map = data ? sMapStore.LookupEntry(data->mapid) : nullptr;
    return data && info && map && !map->Instanceable() && data->id == spawn.entry &&
           data->mapid == spawn.point.mapId && data->phaseMask &&
           RareScoutRules::AcceptsTemplate(info->rank, info->minlevel, info->maxlevel,
               ((data->npcflag | info->npcflag) & UNIT_NPC_FLAG_QUESTGIVER) != 0, true) &&
           Acore::IsValidMapCoord(data->posX, data->posY, data->posZ);
}

Creature* SameRare(CombatState const& state, bool alive)
{
    if (!RareTemplate(state.spawn))
        return nullptr;
    Creature* const creature = ActivityGuards::FindSpawn(state.spawn.spawnId);
    if (!creature || !creature->IsInWorld() || creature->GetMap()->Instanceable() ||
        creature->GetMapId() != state.spawn.point.mapId || creature->GetEntry() != state.spawn.entry ||
        (!state.target.IsEmpty() && creature->GetGUID() != state.target) || (alive && !creature->IsAlive()) ||
        !RareScoutRules::AcceptsTemplate(creature->GetCreatureTemplate()->rank, creature->GetLevel(),
            creature->GetLevel(), creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER), true))
        return nullptr;
    return creature;
}

bool CampBotsSafe(Activity const& activity, CombatState const& state)
{
    CreatureData const* const data = sObjectMgr->GetCreatureData(state.spawn.spawnId);
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In())
            continue;
        Player* const bot = ActivityWorld::FindMember(member);
        if (!member.isBot || !data || !ActivityPvePolicy::SafePlayer(bot) ||
            (bot->GetPhaseMask() & data->phaseMask) == 0 || bot->GetMapId() != state.spawn.point.mapId ||
            bot->GetTeamId() != activity.team || bot->GetLevel() < activity.minLevel ||
            bot->GetLevel() > activity.maxLevel)
            return false;
    }
    return activity.BotsIn() != 0;
}

void CampRetire(ActivityServices& services, Activity& activity, CombatState& state, Player* leader,
                ActivityOutcome outcome, std::string summary)
{
    activity.SetObjective(ObjectGuid::Empty);
    if (!CampBotsSafe(activity, state))
    {
        activity.Finish(ActivityOutcome::Guarded, "rare camp participants became unsafe");
        return;
    }
    float const sight = sPlayerbotAIConfig.sightDistance;
    if (!std::isfinite(sight))
    {
        activity.Finish(ActivityOutcome::Guarded, "rare camp has no safe withdrawal distance");
        return;
    }
    state.escapeDistance = std::max(200.0f, sight + 50.0f);
    Creature* const rare = ActivityGuards::FindSpawn(state.spawn.spawnId);
    float const x = rare ? rare->GetPositionX() : state.spawn.point.x;
    float const y = rare ? rare->GetPositionY() : state.spawn.point.y;
    float dx = leader->GetPositionX() - x;
    float dy = leader->GetPositionY() - y;
    if (dx * dx + dy * dy < 1.0f)
    {
        dx = 1.0f;
        dy = 0.0f;
    }
    float const displacement = std::sqrt(state.spawn.point.SqDist2d(x, y));
    float const offset = state.escapeDistance + displacement + ActivityThreshold::RALLY_ARRIVE_RADIUS + 30.0f;
    state.escape = ActivityGuards::GroundToward(state.spawn.point, state.spawn.point.x + dx,
                                               state.spawn.point.y + dy, offset);
    float const safe = state.escapeDistance + ActivityThreshold::RALLY_ARRIVE_RADIUS;
    if (!state.escape.set || !std::isfinite(state.escape.z) ||
        state.escape.mapId != state.spawn.point.mapId ||
        state.escape.SqDist2d(x, y) <= safe * safe ||
        state.escape.SqDist2d(state.spawn.point.x, state.spawn.point.y) <= safe * safe)
    {
        activity.Finish(ActivityOutcome::Guarded, "rare camp could not find dry safe withdrawal ground");
        return;
    }
    state.retireOutcome = outcome;
    state.retireSummary = std::move(summary);
    state.retireUntilMs = std::min(services.nowMs + RareScoutRules::RETIRE_MS,
                                  state.endMs + RareScoutRules::RETIRE_MS);
    activity.hardDeadlineMs = state.retireUntilMs;
    activity.SetCenter(state.escape, ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_RETREAT);
    activity.SetPhase(ActivityPhase::Resolving, services.nowMs);
}

void CampRetiring(ActivityServices& services, Activity& activity, CombatState const& state)
{
    bool arrived = true;
    bool combat = false;
    Creature* const rare = ActivityGuards::FindSpawn(state.spawn.spawnId);
    float const radius = state.escapeDistance * state.escapeDistance;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In())
            continue;
        Player* const bot = ActivityWorld::FindMember(member);
        if (!bot)
        {
            arrived = false;
            continue;
        }
        arrived = arrived && EscortRules::Arrived(state.escape, bot->GetMapId(), bot->GetPositionX(),
                                                  bot->GetPositionY(), bot->GetPositionZ(),
                                                  ActivityThreshold::RALLY_ARRIVE_RADIUS) &&
                  state.spawn.point.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) > radius &&
                  (!rare || bot->GetExactDist2d(rare) > state.escapeDistance);
        combat = combat || bot->IsInCombat();
    }
    RareScoutProgress const progress = RareScoutRules::RetirementAt(services.nowMs, state.retireUntilMs,
                                                                   arrived, combat);
    if (progress == RareScoutProgress::Success)
        activity.Finish(state.retireOutcome, state.retireSummary + "; group walked away without combat");
    else if (progress == RareScoutProgress::Timeout)
        activity.Finish(ActivityOutcome::Timeout, "rare camp withdrawal could not finish within one minute");
}

class CombatKind final : public ActivityKind
{
public:
    explicit CombatKind(ActivityKindId id) : kind(id) { }
    ActivityKindId Id() const override { return kind; }
    bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
              std::string& why) override
    {
        COA_ASSERT_WORLD_THREAD();
        ActivityConfig const& config = services.config;
        if (kind == ActivityKindId::PvpGang && !request.gm && !config.pvpGangGmVerified)
        {
            why = "a GM must verify both temporary PvP rulesets before ambient patrols";
            return false;
        }
        auto state = std::make_unique<CombatState>();
        bool const camp = kind == ActivityKindId::RareCamp;
        bool const bounty = kind == ActivityKindId::Bounty;
        uint8 team = request.team.value_or(request.nearPlayer ? uint8(request.nearPlayer->GetTeamId()) : 0);
        ActivityPoint rally;
        std::string target = request.target;
        if (camp)
        {
            ActivityHuntTarget const* chosen = target.empty() ? nullptr : config.FindRareCampTarget(target);
            if (!chosen)
                for (auto const& entry : config.rareCampTargets)
                    if (entry.team == team)
                    {
                        chosen = &entry;
                        break;
                    }
            if (!chosen || chosen->team != team || !ActivityGuards::DescribeSpawn(chosen->spawnId, state->spawn))
            {
                why = "choose a known rare-camp target for this faction";
                return false;
            }
            Creature* const creature = SameRare(*state, true);
            if (!creature || ActivityGuards::QuestTargetNeeded(state->spawn.entry, services.realPlayers, nullptr) ||
                HumansNear(services, state->spawn.point, 200))
            {
                why = "rare must be loaded, classic, and clear of nearby players and quest needs";
                return false;
            }
            target = chosen->name;
            state->target = creature->GetGUID();
            rally = ActivityGuards::GroundToward(state->spawn.point, state->spawn.point.x + 100,
                                                 state->spawn.point.y, 45);
        }
        else if (bounty)
        {
            Player* const victim = ObjectAccessor::FindPlayerByName(target);
            if (!victim || !victim->GetSession() || victim->GetSession()->IsBot() ||
                victim->GetTeamId() == team || !victim->IsInWorld())
            {
                why = "bounty target must be an online enemy player";
                return false;
            }
            state->ruleset = victim->HasAura(ActivityThreshold::SPELL_HIGH_RISK) ? ActivityThreshold::SPELL_HIGH_RISK :
                             victim->HasAura(ActivityThreshold::SPELL_WAR_MODE) ? ActivityThreshold::SPELL_WAR_MODE : 0;
            state->target = victim->GetGUID();
            rally = ActivityWorld::PointOf(victim);
        }
        else
        {
            ActivitySpot const* chosen = target.empty() ? nullptr : config.FindPvpGangSpot(target);
            if (!chosen)
                for (auto const& entry : config.pvpGangSpots)
                    if (entry.team == team || entry.team == 2)
                    {
                        chosen = &entry;
                        break;
                    }
            if (!chosen || (chosen->team != team && chosen->team != 2))
            {
                why = "choose a configured PvP roaming spot";
                return false;
            }
            target = chosen->name;
            rally = {chosen->mapId, chosen->x, chosen->y, chosen->z, 0, true};
            state->ruleset = urand(0, 99) < config.pvpGangHighRiskPercent ? ActivityThreshold::SPELL_HIGH_RISK :
                                                                       ActivityThreshold::SPELL_WAR_MODE;
        }
        uint32 const minimum = camp ? config.rareCampMinBots : bounty ? config.bountyMinBots : config.pvpGangMinBots;
        uint32 const maximum = camp ? config.rareCampMaxBots : bounty ? config.bountyMaxBots : config.pvpGangMaxBots;
        uint32 const nearby = camp ? config.rareCampNearbyYards :
                              bounty ? config.bountyNearbyYards : config.pvpGangNearbyYards;
        if (!rally.set || request.capacity < minimum)
        {
            why = "not enough capacity or no valid rally";
            return false;
        }
        state->anchor = rally;
        ActivityCandidateQuery query;
        query.team = team;
        uint8 const level = request.level ? request.level : camp ? state->spawn.level : 60;
        query.minLevel = uint8(std::max<int32>(1, int32(level) - 5));
        query.maxLevel = uint8(std::min<uint32>(60, uint32(level) + 5));
        query.center = rally;
        query.sameMapOnly = true;
        query.maxDistance = float(nearby);
        query.limit = 2 * maximum;
        auto candidates = services.candidates.Find(query);
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](auto const& candidate)
        {
            Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
            if (camp)
                return !ActivityPvePolicy::SafePlayer(bot);
            return !bot || bot->HasAura(ActivityThreshold::SPELL_MERCENARY) ||
                   (bounty && !ActivityPvpPolicy::Eligible(bot, ObjectAccessor::FindConnectedPlayer(state->target),
                                                         state->ruleset, false));
        }), candidates.end());
        if (candidates.size() < minimum)
        {
            why = "too few nearby eligible bots";
            return false;
        }
        plan.requester = request.nearPlayer ? request.nearPlayer->GetGUID() : ObjectGuid::Empty;
        plan.kind = kind;
        plan.team = team;
        plan.minLevel = query.minLevel;
        plan.maxLevel = query.maxLevel;
        plan.target = target;
        plan.name = target;
        plan.rally = rally;
        plan.needed = minimum;
        plan.wanted = std::min<uint32>(request.capacity,
                                      request.size ? std::clamp(request.size, minimum, maximum) : maximum);
        plan.leaderFixed = true;
        plan.candidates = std::move(candidates);
        plan.state = std::move(state);
        return true;
    }

    bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) override
    {
        COA_ASSERT_WORLD_THREAD();
        auto* const state = dynamic_cast<CombatState*>(plan.state.get());
        bool const camp = kind == ActivityKindId::RareCamp;
        if (!state || !services.config.KindEnabled(kind) || plan.kind != kind ||
            plan.decisions.size() != plan.candidates.size() || plan.candidates.empty() ||
            plan.decisions.front() != 1 || (camp && (!SameRare(*state, true) ||
                ActivityGuards::QuestTargetNeeded(state->spawn.entry, services.realPlayers, nullptr) ||
                HumansNear(services, state->spawn.point, 200))))
            return false;
        std::vector<ActivityCandidate const*> accepted;
        for (std::size_t i = 0; i < plan.candidates.size() && activity.BotsIn() < plan.wanted; ++i)
            if (plan.decisions[i] == 1)
            {
                auto const& candidate = plan.candidates[i];
                Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
                PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
                uint32 const nearby = camp ? services.config.rareCampNearbyYards :
                    kind == ActivityKindId::Bounty ? services.config.bountyNearbyYards :
                                                   services.config.pvpGangNearbyYards;
                Creature* const rare = camp ? SameRare(*state, true) : nullptr;
                bool const valid = ai && ai->GetCoaEpoch() == candidate.epoch &&
                    ActivityCandidates::IsFree(bot) && !bot->IsInCombat() && bot->GetTeamId() == plan.team &&
                    bot->GetLevel() >= plan.minLevel && bot->GetLevel() <= plan.maxLevel &&
                    bot->GetMapId() == plan.rally.mapId && !bot->GetMap()->Instanceable() &&
                    bot->GetExactDist(plan.rally.x, plan.rally.y, plan.rally.z) <= nearby &&
                    !bot->HasAura(ActivityThreshold::SPELL_MERCENARY) &&
                    (camp ? rare && ActivityPvePolicy::SafePlayer(bot) && bot->InSamePhase(rare) :
                     state->ruleset == ActivityThreshold::SPELL_WAR_MODE ||
                     state->ruleset == ActivityThreshold::SPELL_HIGH_RISK);
                if (!valid)
                {
                    if (!i)
                        return false;
                    continue;
                }
                if (kind == ActivityKindId::Bounty &&
                    !ActivityPvpPolicy::Eligible(bot, ObjectAccessor::FindConnectedPlayer(state->target),
                                                 state->ruleset, false))
                {
                    if (!i)
                        return false;
                    continue;
                }
                accepted.push_back(&candidate);
                if (accepted.size() >= plan.wanted)
                    break;
            }
        if (accepted.size() < plan.needed)
            return false;
        activity.leader = accepted.front()->guid;
        for (auto const* candidate : accepted)
            activity.Enroll(candidate->guid, candidate->epoch, true, candidate->level, services.nowUnix);
        activity.name = plan.name;
        activity.target = plan.target;
        activity.minLevel = plan.minLevel;
        activity.maxLevel = plan.maxLevel;
        activity.SetCenter(plan.rally, 40);
        activity.SetTravel(ActivityTravel::None);
        activity.SetFlags(ACTIVITY_VIEW_HOLD);
        activity.SetPhase(ActivityPhase::Forming, services.nowMs);
        activity.pvpRuleset = state->ruleset;
        uint32 const minutes = kind == ActivityKindId::RareCamp ? services.config.rareCampMinutes :
                               kind == ActivityKindId::Bounty ? services.config.bountyMinutes :
                                                               services.config.pvpGangMinutes;
        state->endMs = services.nowMs + uint64(minutes) * 60000;
        state->minimum = plan.needed;
        state->gatherUntilMs = services.nowMs + 180000;
        activity.hardDeadlineMs = state->endMs + (camp ? RareScoutRules::RETIRE_MS :
            std::max<uint64>(5000, uint64(services.config.tickMs) + 1000));
        for (auto const& member : activity.members)
            if (member.guid != activity.leader)
                state->toAdd.push_back(member.guid);
        activity.state = std::move(plan.state);
        return true;
    }

    void Tick(ActivityServices& services, Activity& activity) override
    {
        COA_ASSERT_WORLD_THREAD();
        if (activity.finishing)
            return;
        auto* const state = dynamic_cast<CombatState*>(activity.state.get());
        ActivityMember const* const leaderMember = activity.Leader();
        Player* const leader = leaderMember && leaderMember->In() ? ActivityWorld::FindMember(*leaderMember) : nullptr;
        if (!state || !leader || !leader->IsAlive())
        {
            activity.SetObjective(ObjectGuid::Empty);
            activity.Finish(ActivityOutcome::LeaderGone, "combat group lost its enrolled living leader");
            return;
        }
        bool const camp = kind == ActivityKindId::RareCamp;
        if (!CurrentBotSafe(activity, *leaderMember, leader, leader) || leader->GetMapId() != state->anchor.mapId)
        {
            activity.SetObjective(ObjectGuid::Empty);
            activity.Finish(ActivityOutcome::Guarded, "combat group leader became unsafe or changed map");
            return;
        }
        Group* group = state->grouped && !activity.groupGuid.IsEmpty() ?
            sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter()) : nullptr;
        if (state->grouped && (!group || group->GetLeaderGUID() != activity.leader ||
            leader->GetGroup() != group || !group->IsMember(activity.leader)))
        {
            activity.SetObjective(ObjectGuid::Empty);
            activity.Finish(ActivityOutcome::LeaderGone, "combat group lost its native group or leader");
            return;
        }
        for (ActivityMember const& member : activity.members)
        {
            if (!member.In() || member.guid == activity.leader)
                continue;
            Player* const bot = ActivityWorld::FindMember(member);  // resolves the enrolled epoch, never a relog
            bool const pending = activity.phase == ActivityPhase::Gathering &&
                std::find(state->toAdd.begin(), state->toAdd.end(), member.guid) != state->toAdd.end();
            ActivityLeaveReason reason = ActivityLeaveReason::None;
            if (!bot)
                reason = ActivityLeaveReason::Gone;
            else if (!bot->IsAlive())
                reason = ActivityLeaveReason::Dropped;
            else if (!CurrentBotSafe(activity, member, bot, leader) ||
                     (!camp && member.rulesetApplied && !RulesetReady(bot, member, state->ruleset)))
                reason = ActivityLeaveReason::Kicked;
            else if (state->grouped &&
                     ((bot->GetGroup() && bot->GetGroup() != group) ||
                      (!pending && (bot->GetGroup() != group || !group->IsMember(member.guid)))))
                reason = ActivityLeaveReason::Grouped;
            if (reason != ActivityLeaveReason::None)
                activity.MarkLeft(member.guid, reason, services.nowUnix);
        }
        state->toAdd.erase(std::remove_if(state->toAdd.begin(), state->toAdd.end(), [&](ObjectGuid guid)
        {
            ActivityMember const* const member = activity.FindMember(guid);
            return !member || !member->In() || (group && group->IsMember(guid));
        }), state->toAdd.end());
        if (!state->minimum || activity.BotsIn() < state->minimum)
        {
            activity.SetObjective(ObjectGuid::Empty);
            if (camp && state->retireOutcome != ActivityOutcome::None)
            {
                state->retireOutcome = ActivityOutcome::TooFew;
                state->retireSummary = "combat group dropped below its required living members";
                CampRetiring(services, activity, *state);
            }
            else if (camp && state->grouped && activity.BotsIn())
                CampRetire(services, activity, *state, leader, ActivityOutcome::TooFew,
                           "combat group dropped below its required living members");
            else
                activity.Finish(ActivityOutcome::TooFew, "combat group dropped below its required living members");
            return;
        }
        if (!camp && leaderMember->rulesetApplied && !RulesetReady(leader, *leaderMember, state->ruleset))
        {
            activity.SetObjective(ObjectGuid::Empty);
            activity.Finish(ActivityOutcome::Guarded, "combat group leader lost its applied ruleset");
            return;
        }
        if (!camp &&
            leader->GetExactDist(state->anchor.x, state->anchor.y, state->anchor.z) >
                float(kind == ActivityKindId::Bounty ? services.config.bountyNearbyYards : services.config.pvpGangNearbyYards))
        {
            activity.SetObjective(ObjectGuid::Empty);
            activity.Finish(ActivityOutcome::Guarded, "combat group left its starting area");
            return;
        }
        if (camp && !CampBotsSafe(activity, *state))
        {
            activity.SetObjective(ObjectGuid::Empty);
            activity.Finish(ActivityOutcome::Guarded, "rare camp member left PvE safety");
            return;
        }
        if (camp && state->retireOutcome != ActivityOutcome::None)
        {
            CampRetiring(services, activity, *state);
            return;
        }
        if (camp && (ActivityGuards::QuestTargetNeeded(state->spawn.entry, services.realPlayers, nullptr) ||
                     HumansNear(services, state->spawn.point, 200)))
        {
            CampRetire(services, activity, *state, leader, ActivityOutcome::Guarded,
                       "rare left to nearby players or quest holders");
            return;
        }
        if (camp && !RareTemplate(state->spawn))
        {
            CampRetire(services, activity, *state, leader, ActivityOutcome::Guarded,
                       "rare template changed or became unsafe");
            return;
        }
        if (!state->grouped)
        {
            std::vector<ObjectGuid> added, skipped;
            ActivityOpResult const result = services.ops.FormGroup(activity.leader, state->toAdd, GROUP_LOOT, 2,
                                                                   activity.groupGuid, added, skipped);
            if (result == ActivityOpResult::Deferred)
                return;
            if (result != ActivityOpResult::Done)
            {
                activity.Finish(ActivityOutcome::TooFew, "group formation failed");
                return;
            }
            state->grouped = true;
            state->toAdd = std::move(skipped);
            group = sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
            if (!group || leader->GetGroup() != group || group->GetLeaderGUID() != activity.leader ||
                !group->IsMember(activity.leader))
            {
                activity.Finish(ActivityOutcome::LeaderGone, "formed combat group has no enrolled native leader");
                return;
            }
            activity.SetFlags(0);  // HOLD must not prevent the leader from walking to the gathering point.
            activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
        }
        if (!state->toAdd.empty())
        {
            if (services.nowMs >= state->gatherUntilMs)
            {
                activity.Finish(ActivityOutcome::TooFew, "combat group member addition timed out");
                return;
            }
            ActivityOpResult const result = services.ops.AddMember(activity.groupGuid, activity.leader,
                                                                   state->toAdd.front());
            if (result == ActivityOpResult::Done)
                state->toAdd.erase(state->toAdd.begin());
            else if (result != ActivityOpResult::Deferred && result != ActivityOpResult::Refused)
            {
                activity.MarkLeft(state->toAdd.front(), ActivityLeaveReason::Dropped, services.nowUnix);
                state->toAdd.erase(state->toAdd.begin());
                if (activity.BotsIn() < state->minimum)
                    activity.Finish(ActivityOutcome::TooFew, "combat group member could not be added");
            }
            return;
        }
        uint32 nativeMembers = 0;
        bool rulesetsReady = true;
        for (ActivityMember const& member : activity.members)
            if (member.In() && member.isBot)
            {
                Player* const bot = ActivityWorld::FindMember(member);
                if (bot && bot->IsAlive() && bot->GetGroup() == group && group->IsMember(member.guid))
                    ++nativeMembers;
                rulesetsReady = rulesetsReady && bot && RulesetReady(bot, member, state->ruleset);
            }
        bool const nativeReady = ActivityCombatRules::ReadyGroup(state->minimum, activity.BotsIn(), nativeMembers,
            leaderMember->In(), group->GetLeaderGUID() == activity.leader, !state->toAdd.empty());
        if (!nativeReady || (activity.phase != ActivityPhase::Gathering && !rulesetsReady))
        {
            activity.SetObjective(ObjectGuid::Empty);
            activity.Finish(ActivityOutcome::TooFew, "combat group lost current native membership or rulesets");
            return;
        }
        if (services.nowMs >= state->endMs)
        {
            if (camp)
            {
                CampRetire(services, activity, *state, leader, ActivityOutcome::Timeout,
                           "rare camp reached its wait deadline without a confirmed kill");
                return;
            }
            bool const patrolled = kind == ActivityKindId::PvpGang &&
                (activity.phase == ActivityPhase::Marching || activity.phase == ActivityPhase::Active) &&
                rulesetsReady && ActivityCombatRules::CompletedPatrol(state->minimum, activity.BotsIn(), nativeMembers,
                    leaderMember->In(), group->GetLeaderGUID() == activity.leader, !state->toAdd.empty(),
                    state->patrolObserved);
            activity.Finish(patrolled ? ActivityOutcome::Success : ActivityOutcome::Timeout,
                            patrolled ? "PvP patrol completed" : "combat group did not complete its objective");
            return;
        }
        if (activity.phase == ActivityPhase::Gathering)
        {
            bool ready = rulesetsReady && nativeReady;
            for (auto const& member : activity.members)
                if (member.In())
                {
                    Player* const bot = ActivityWorld::FindMember(member);
                    ready = ready && bot && bot->GetMap() == leader->GetMap() &&
                            bot->GetExactDist(activity.center.x, activity.center.y, activity.center.z) < 45;
                }
            if (!ready)
            {
                if (services.nowMs >= state->gatherUntilMs)
                    activity.Finish(ActivityOutcome::TooFew, "nearby walking gather timed out");
                return;
            }
            activity.SetFlags(0);
            activity.SetPhase(ActivityPhase::Active, services.nowMs);
        }
        if (camp)
        {
            Creature* const creature = ActivityGuards::FindSpawn(state->spawn.spawnId);
            if (!creature)
            {
                CampRetire(services, activity, *state, leader, ActivityOutcome::Guarded,
                           "rare unloaded; no kill inferred and replacement not attacked");
                return;
            }
            if (!SameRare(*state, false) || !leader->InSamePhase(creature) || activity.BotsIn() < state->minimum)
            {
                CampRetire(services, activity, *state, leader, ActivityOutcome::Guarded,
                           "rare identity/phase or group changed; replacement not attacked");
                return;
            }
            if (creature->IsAlive())
            {
                activity.SetObjective(state->target);
                if (creature->GetVictim() && creature->GetLootRecipientGroup() == leader->GetGroup())
                    state->engaged = true;
            }
            else if (state->engaged && creature->GetGUID() == state->target &&
                     creature->GetLootRecipientGroup() == leader->GetGroup())
            {
                CampRetire(services, activity, *state, leader, ActivityOutcome::Success,
                           "same rare killed by this group; corpse left unlooted");
            }
            else
                CampRetire(services, activity, *state, leader, ActivityOutcome::Guarded,
                           "rare died without this group's confirmed engagement");
            return;
        }
        if (kind == ActivityKindId::Bounty)
        {
            Player* const victim = ObjectAccessor::FindConnectedPlayer(state->target);
            if (ActivityPvpPolicy::Killed(activity.id, state->target))
            {
                activity.Finish(ActivityOutcome::Success, "bounty target killed; no gold reward");
                return;
            }
            if (!ActivityPvpPolicy::Eligible(leader, victim, state->ruleset) ||
                leader->GetExactDist(victim) > float(services.config.bountyNearbyYards))
            {
                activity.Finish(ActivityOutcome::Guarded, "target no longer eligible or nearby");
                return;
            }
            activity.SetObjective(state->target);
            activity.SetCenter(ActivityWorld::PointOf(victim), 15);
            activity.SetPhase(leader->GetExactDist(victim) > 45 ? ActivityPhase::Marching : ActivityPhase::Active,
                              services.nowMs);
            return;
        }
        if (services.nowMs < state->nextScanMs)
            return;
        state->nextScanMs = services.nowMs + 5000;
        Player* target = nullptr;
        float closest = 120;
        auto consider = [&](Player* player)
        {
            if (ActivityPvpPolicy::Eligible(leader, player, state->ruleset) &&
                player->GetExactDist(state->anchor.x, state->anchor.y, state->anchor.z) <=
                    float(services.config.pvpGangNearbyYards) &&
                player->GetExactDist(leader) < closest)
            {
                closest = player->GetExactDist(leader);
                target = player;
            }
        };
        for (Player* const player : services.realPlayers)
            consider(player);
        for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
            consider(it->second);
        activity.SetObjective(target ? target->GetGUID() : ObjectGuid::Empty);
        if (!target)
        {
            float const angle = float((++state->waypoint % 8)) * 0.78539816f;
            ActivityPoint roam = activity.center;
            roam.x = state->anchor.x + 80 * std::cos(angle);
            roam.y = state->anchor.y + 80 * std::sin(angle);
            roam = ActivityWorld::GroundNear(roam, 3);
            if (roam.set)
            {
                activity.SetCenter(roam, 40);
                activity.SetPhase(ActivityPhase::Marching, services.nowMs);
                state->patrolObserved = true;
            }
        }
        else
        {
            // Walk into native engage range instead of waiting at the patrol anchor.
            activity.SetCenter(ActivityWorld::PointOf(target), 15);
            activity.SetPhase(closest > 45 ? ActivityPhase::Marching : ActivityPhase::Active, services.nowMs);
            state->patrolObserved = true;
        }
    }

    bool Accepts(ActivityServices&, Activity const&, Player*) override { return false; }
    std::string Join(ActivityServices&, Activity&, Player*) override
    {
        return ActivityWorld::Line("coa_act_combat_closed", "This bot combat group does not recruit players.");
    }
    void OnEnd(ActivityServices&, Activity& activity) override { ActivityPvpPolicy::Release(activity.id); }
    std::string Describe(Activity const& activity, uint64) const override
    {
        return Acore::StringFormat("walking combat group, objective {}, ruleset {}", activity.objective.GetCounter(),
                                   activity.pvpRuleset);
    }
    std::optional<std::vector<std::string>> Targets(ActivityServices& services, ActivityStartRequest const&) override
    {
        if (kind == ActivityKindId::Bounty)
            return std::vector<std::string>{}; // explicit target only
        std::vector<std::string> targets;
        if (kind == ActivityKindId::RareCamp)
            for (auto const& target : services.config.rareCampTargets)
                targets.push_back(target.name);
        else
            for (auto const& spot : services.config.pvpGangSpots)
                targets.push_back(spot.name);
        return targets;
    }
private:
    ActivityKindId kind;
};
}

bool ActivityCombatRules::ReadyGroup(uint32 minimum, uint32 enrolled, uint32 nativeMembers, bool leaderIn,
                                   bool nativeLeader, bool pendingAdds)
{
    return minimum && enrolled >= minimum && nativeMembers == enrolled && leaderIn && nativeLeader && !pendingAdds;
}

bool ActivityCombatRules::CompletedPatrol(uint32 minimum, uint32 enrolled, uint32 nativeMembers, bool leaderIn,
                                        bool nativeLeader, bool pendingAdds, bool patrolObserved)
{
    return ReadyGroup(minimum, enrolled, nativeMembers, leaderIn, nativeLeader, pendingAdds) && patrolObserved;
}

bool ActivityCombatRules::SelfTest()
{
    return ReadyGroup(3, 3, 3, true, true, false) && ReadyGroup(3, 4, 4, true, true, false) &&
        !ReadyGroup(3, 0, 0, false, false, false) &&  // native #50/#51 empty group must never gather or succeed
        !ReadyGroup(3, 2, 2, true, true, false) && !ReadyGroup(0, 0, 0, true, true, false) &&
        !ReadyGroup(3, 3, 2, true, true, false) && !ReadyGroup(3, 3, 3, false, true, false) &&
        !ReadyGroup(3, 3, 3, true, false, false) && !ReadyGroup(3, 3, 3, true, true, true) &&
        CompletedPatrol(3, 3, 3, true, true, false, true) &&
        !CompletedPatrol(3, 3, 3, true, true, false, false) &&
        !CompletedPatrol(3, 0, 0, false, false, false, true) &&
        !CompletedPatrol(3, 2, 2, true, true, false, true) &&
        !CompletedPatrol(3, 3, 2, true, true, false, true) &&
        !CompletedPatrol(3, 3, 3, false, true, false, true) &&
        !CompletedPatrol(3, 3, 3, true, false, false, true) &&
        !CompletedPatrol(3, 3, 3, true, true, true, true);
}

std::unique_ptr<ActivityKind> CreatePvpGangKind() { return std::make_unique<CombatKind>(ActivityKindId::PvpGang); }
std::unique_ptr<ActivityKind> CreateBountyKind() { return std::make_unique<CombatKind>(ActivityKindId::Bounty); }
std::unique_ptr<ActivityKind> CreateRareCampKind() { return std::make_unique<CombatKind>(ActivityKindId::RareCamp); }
