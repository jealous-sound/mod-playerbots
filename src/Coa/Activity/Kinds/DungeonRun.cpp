/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonRun.h"
#include "ActivityBrain.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "ActivityOrganizer.h"
#include "ActivityStore.h"
#include "CoaPlayerPrefs.h"
#include "CoaSpecialization.h"
#include "CoaThread.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace
{
constexpr char const* LOG = "playerbots.activity";
constexpr uint32 SEAT_TRIES = 12;             // FindCoaRecruit calls per seat at most
constexpr uint64 QUEUE_RETRY_MS = 30000;      // a failed Dungeon Finder attempt is tried again this much later
constexpr float PORTAL_ARRIVE_YARDS = 25.0f;  // Walk: the leader reached the portal
constexpr float PORTAL_PLAYER_YARDS = 40.0f;  // Portal: a real member this close to it is followed in
constexpr uint64 ENTER_GRACE_MS = 90000;      // Inside: bots still outside this long after the player went in ...
constexpr uint64 ENTER_RETRY_MS = 5000;       // ... are teleported to the entrance, once every this at most
constexpr uint64 OUTSIDE_END_MS = 600000;     // Inside: no real member in the instance this long ends the run
constexpr uint64 LINE_RETRY_MS = 15000;       // a line is retried this long (budget), then dropped
constexpr uint32 MIN_PLAYER_LEVEL = 10;       // runs are planned for players of this level and above
constexpr float CENTER_RADIUS = 30.0f;        // Begin and Portal: SetCenter(point, 30)
constexpr uint64 POST_RETRY_MS = 60000;       // a recruitment post refused or failed is tried again this much later
constexpr uint32 PARTY_SIZE = 5;

// A party line the leader still has to say (DungeonRunState::lineDue).
enum class DungeonLine : uint8
{
    None = 0,
    Queue,   // coa_act_dungeon_queue, at the first Dungeon Finder attempt
    Thanks,  // coa_act_dungeon_thanks, entering Idle once a real member was inside
};

char const* const LFM_KEYS[2] = {"coa_act_lfm_dungeon", "coa_act_lfm_dungeon_alt"};
char const* const LFM_ENGLISH[2] = {"LFM %dungeon, need %role. Whisper me!",
                                    "Group for %dungeon, still missing %role. Whisper me inv."};

char const* TeamName(uint8 team) { return team == TEAM_ALLIANCE ? "alliance" : "horde"; }

char const* RoleName(uint8 role)
{
    switch (CoaRole(role))
    {
        case CoaRole::Tank:
            return "tank";
        case CoaRole::Heal:
            return "healer";
        default:
            return "dps";
    }
}

std::string RoleNames(std::vector<uint8> const& roles)
{
    std::string out;
    for (uint8 const role : roles)
        out += std::string(out.empty() ? "" : ", ") + RoleName(role);
    return out.empty() ? "none" : out;
}

// "a tank, a DPS": the open roles in the realm's words (%role).
std::string RoleWords(std::vector<uint8> const& roles)
{
    std::string out;
    for (uint8 const role : roles)
    {
        std::string word;
        switch (CoaRole(role))
        {
            case CoaRole::Tank:
                word = ActivityWorld::Line("coa_act_role_tank", "a tank");
                break;
            case CoaRole::Heal:
                word = ActivityWorld::Line("coa_act_role_heal", "a healer");
                break;
            default:
                word = ActivityWorld::Line("coa_act_role_dps", "a DPS");
                break;
        }
        out += (out.empty() ? "" : ", ") + word;
    }
    return out;
}

// A spoken line may start with a placeholder (the dungeon's name): the first ASCII letter goes upper case.
std::string Spoken(std::string text)
{
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
        text[0] = char(text[0] - 'a' + 'A');
    return text;
}

// A zone in English words (the brain's `where`), whatever the realm's DBC locale.
std::string EnglishZoneName(uint32 zoneId)
{
    AreaTableEntry const* const entry = zoneId ? sAreaTableStore.LookupEntry(zoneId) : nullptr;
    if (!entry)
        return "";
    char const* name = entry->area_name[LOCALE_enUS];
    if (!name || !*name)
        name = entry->area_name[sWorld->GetDefaultDbcLocale()];
    return name ? name : "";
}

DungeonRunState* StateOf(Activity& activity) { return dynamic_cast<DungeonRunState*>(activity.state.get()); }

DungeonRunState const* StateOf(Activity const& activity)
{
    return dynamic_cast<DungeonRunState const*>(activity.state.get());
}

// The open roles no pending invitation keeps (what an LFM asks for, what a join may take).
std::vector<uint8> UnheldOpen(DungeonRunState const& state)
{
    std::vector<uint8> open = state.openRoles;
    for (auto const& [player, role] : state.heldSeats)
    {
        auto const it = std::find(open.begin(), open.end(), role);
        if (it != open.end())
            open.erase(it);
    }
    return open;
}

Group* RunGroup(Activity const& activity)
{
    return activity.groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
}

// Room for one more invitation: five at most, pending invitations counted (InvitePlayer would turn a full party into
// a raid, which the Dungeon Finder refuses).
bool HasRoom(Activity const& activity, DungeonRunState const& state)
{
    Group* group = RunGroup(activity);
    return group && group->GetMembersCount() + state.heldSeats.size() < PARTY_SIZE;
}

// Whether the run takes this real player now: recruiting, room left, the player ungrouped, its role open, its level
// in the dungeon's range.
bool Takes(Activity const& activity, DungeonRunState const& state, Player* player)
{
    if (!player || activity.finishing || state.step != DungeonStep::Recruit || player->GetGroup() ||
        !HasRoom(activity, state))
        return false;
    uint32 const level = player->GetLevel();
    if (level < state.dungeon.minLevel || level > state.dungeon.maxLevel)
        return false;
    std::vector<uint8> const open = UnheldOpen(state);
    return std::find(open.begin(), open.end(), DungeonLfg::RoleOf(player)) != open.end();
}

// The walking fallback's portal, when it is known and not in the other faction's territory (an Alliance group is not
// sent to the Ragefire Chasm portal in Orgrimmar).
bool PortalUsable(DungeonInfo const& dungeon, uint8 team)
{
    if (!dungeon.portal.set)
        return false;
    AreaTableEntry const* zone = dungeon.portal.zoneId ? sAreaTableStore.LookupEntry(dungeon.portal.zoneId) : nullptr;
    if (!zone)
        return true;
    return !((zone->team == AREATEAM_HORDE && team == TEAM_ALLIANCE) ||
             (zone->team == AREATEAM_ALLY && team == TEAM_HORDE));
}

// DungeonLfg::Pick among the dungeons whose portal the group can walk to (no Dungeon Finder): a portal on the
// player's continent first (the player walks there too), then the target level nearest `level`, ties urand.
std::optional<DungeonInfo> PickWalkable(std::vector<uint32> const& allowed, uint8 level, uint8 team, uint32 mapId)
{
    std::vector<DungeonInfo const*> best;
    int32 bestGap = std::numeric_limits<int32>::max();
    for (DungeonInfo const& dungeon : DungeonLfg::All())
    {
        if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), dungeon.id) == allowed.end())
            continue;
        if (level < dungeon.minLevel || level > dungeon.maxLevel || !PortalUsable(dungeon, team))
            continue;
        int32 const otherContinent = dungeon.portal.mapId != mapId ? 1000 : 0;
        int32 const gap = otherContinent + std::abs(int32(dungeon.targetLevel) - int32(level));
        if (gap < bestGap)
        {
            best.clear();
            bestGap = gap;
        }
        if (gap == bestGap)
            best.push_back(&dungeon);
    }
    if (best.empty())
        return std::nullopt;
    return *best[urand(0, uint32(best.size()) - 1)];
}

// Why this player cannot have a run planned for it now; null when it can.
char const* PlayerUnfit(Player* player, ActivityStartRequest const& request)
{
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || !player->GetSession())
        return "not in the world";
    if (player->GetSession()->IsBot())
        return "not a real player";
    if (!player->IsAlive())
        return "dead";
    if (player->GetGroup())
        return "in a group";
    if (DungeonLfg::State(player->GetGUID()) != DungeonQueueState::None)
        return "at the Dungeon Finder";
    if (player->InBattleground() || player->InArena() || player->InBattlegroundQueue())
        return "in a battleground or an arena, or queued for one";
    if (player->IsInFlight())
        return "in flight";
    if (!player->GetMap() || player->GetMap()->Instanceable())
        return "in an instance";
    if (player->GetLevel() < MIN_PLAYER_LEVEL)
        return "below level 10";
    if (request.team && *request.team != uint8(player->GetTeamId()))
        return "of the other faction";
    return nullptr;
}

// The seat-0 bot among the fitting ones found: in the player's zone first (it posts in the zone's channels the
// player reads), then the nearest on the player's map, then the first found.
Player* BestLeader(std::vector<Player*> const& found, Player* player)
{
    Player* best = nullptr;
    int32 bestRank = 3;
    float bestDistance = 0.0f;
    for (Player* bot : found)
    {
        bool const sameMap = bot->GetMapId() == player->GetMapId();
        int32 const rank = !sameMap ? 2 : bot->GetZoneId() == player->GetZoneId() ? 0 : 1;
        float const distance = sameMap ? bot->GetExactDist2d(player) : 0.0f;
        if (!best || rank < bestRank || (rank == bestRank && rank < 2 && distance < bestDistance))
        {
            best = bot;
            bestRank = rank;
            bestDistance = distance;
        }
    }
    return best;
}

Player* InWorld(ObjectGuid guid)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    return player && player->IsInWorld() && !player->IsDuringRemoveFromWorld() ? player : nullptr;
}

Player* LeaderOf(Activity& activity)
{
    ActivityMember* member = activity.Leader();
    return member && member->In() ? ActivityWorld::FindMember(*member) : nullptr;
}

// Who says the run's lines: the leader, else the first bot still in (the leader may be gone once inside).
ObjectGuid Speaker(Activity& activity)
{
    if (LeaderOf(activity))
        return activity.leader;
    for (ActivityMember const& member : activity.members)
        if (member.isBot && member.In() && ActivityWorld::FindMember(member))
            return member.guid;
    return ObjectGuid::Empty;
}

// The first real member in the dungeon's instance map.
Player* RealMemberOn(Activity& activity, uint32 mapId)
{
    for (ActivityMember const& member : activity.members)
    {
        if (member.isBot || !member.In())
            continue;
        Player* player = ActivityWorld::FindMember(member);
        if (player && player->GetMapId() == mapId)
            return player;
    }
    return nullptr;
}

void GoStep(Activity& activity, DungeonRunState& state, DungeonStep step, uint64 nowMs)
{
    LOG_DEBUG(LOG, "activity #{} dungeon: step {} -> {}", activity.id, DungeonStepName(state.step),
              DungeonStepName(step));
    state.step = step;
    state.stepSinceMs = nowMs;
}

void LineDue(DungeonRunState& state, DungeonLine line, uint64 nowMs)
{
    state.lineDue = uint8(line);
    state.lineDueUntilMs = nowMs + LINE_RETRY_MS;
}

// The due party line; the budget delays it for LINE_RETRY_MS at most.
void SayDue(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    DungeonLine const line = DungeonLine(state.lineDue);
    if (line == DungeonLine::None)
        return;
    if (services.nowMs >= state.lineDueUntilMs)
    {
        LOG_DEBUG(LOG, "activity #{} dungeon: line {} given up", activity.id, uint32(state.lineDue));
        state.lineDue = uint8(DungeonLine::None);
        return;
    }
    ObjectGuid const speaker = Speaker(activity);
    if (speaker.IsEmpty())
        return;

    std::string text;
    if (line == DungeonLine::Queue)
        text = ActivityWorld::Line("coa_act_dungeon_queue",
                                   "Everyone is here: queueing us for %dungeon, confirm your role.",
                                   {{"%dungeon", activity.name}});
    else
        text = ActivityWorld::Line("coa_act_dungeon_thanks", "Thanks for the run, see you around!",
                                   {{"%dungeon", activity.name}});
    ActivityOpResult const result =
        services.ops.PostToChannel(speaker, ActivityChatTarget::Party, Spoken(std::move(text)));
    if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
        return;
    state.lineDue = uint8(DungeonLine::None);
}

// The walking fallback: the leader whispers every real member where to meet (once).
void TellMeet(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    if (state.meetLineSaid || state.step != DungeonStep::Walk)
        return;
    if (services.nowMs >= state.meetUntilMs)
    {
        state.meetLineSaid = true;
        return;
    }
    std::string const text = Spoken(ActivityWorld::Line(
        "coa_act_dungeon_meet", "The finder is not helping. Meet us at the %dungeon entrance in %zone.",
        {{"%dungeon", activity.name}, {"%zone", ActivityWorld::AreaName(state.dungeon.portal.zoneId)}}));
    for (ActivityMember const& member : activity.members)
    {
        if (member.isBot || !member.In() || state.meetTold.count(member.guid))
            continue;
        ActivityOpResult const result =
            services.ops.PostToChannel(activity.leader, ActivityChatTarget::Whisper, text, member.guid);
        if (result == ActivityOpResult::Deferred)
            return;
        state.meetTold.insert(member.guid);
    }
    state.meetLineSaid = true;
}

void EnterRecruit(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    uint64 const now = services.nowMs;
    GoStep(activity, state, DungeonStep::Recruit, now);
    state.recruitUntilMs = now + uint64(services.config.dungeonRecruitMinutes) * MINUTE * IN_MILLISECONDS;
    state.nextPostMs = now;
    activity.SetFlags(ACTIVITY_VIEW_RECRUITING);
    activity.SetPhase(ActivityPhase::Recruiting, now);
}

void EnterQueue(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    uint64 const now = services.nowMs;
    GoStep(activity, state, DungeonStep::Queue, now);
    state.lfgTries = 0;
    state.retryAtMs = now;
    state.queueUntilMs = 0;
    state.dungeonSeenMs = 0;
    state.lfgPending = false;
    // Nothing moves the bots now: the finder sends everyone in.
    activity.SetFlags(0);
    activity.SetCenter(ActivityPoint(), 0.0f);
    activity.SetTravel(ActivityTravel::None);
    activity.SetPhase(ActivityPhase::Gathering, now);
}

// The group walks to the portal when it can (no Dungeon Finder, or it failed); ends the run otherwise.
void WalkOrFinish(ActivityServices& services, Activity& activity, DungeonRunState& state, char const* why)
{
    if (!PortalUsable(state.dungeon, activity.team))
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: {}", activity.name, why));
        return;
    }
    uint64 const now = services.nowMs;
    GoStep(activity, state, DungeonStep::Walk, now);
    state.meetUntilMs = now + LINE_RETRY_MS;
    activity.SetCenter(state.dungeon.portal, PORTAL_ARRIVE_YARDS);
    activity.SetTravel(ActivityTravel::FollowLeader);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Marching, now);
}

void EnterPortal(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    GoStep(activity, state, DungeonStep::Portal, services.nowMs);
    activity.SetCenter(state.dungeon.portal, CENTER_RADIUS);
    activity.SetTravel(ActivityTravel::FollowLeader);
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
}

// Every bot takes this real member as its master (the director's member pass), the lead goes to it once, and the
// "coa activity" movement stays quiet: the bots behave as any bot of a player's group.
void FollowPlayer(ActivityServices& services, Activity& activity, DungeonRunState& state, Player* player)
{
    activity.SetFollow(player->GetGUID());
    if (!state.leadHanded && !activity.groupGuid.IsEmpty())
    {
        ActivityOpResult const result = services.ops.HandLeader(activity.groupGuid, player->GetGUID());
        LOG_DEBUG(LOG, "activity #{} dungeon: lead to player {} {}", activity.id, player->GetGUID().GetCounter(),
                  ActivityOpResultName(result));
        if (result != ActivityOpResult::Gone)
            state.leadHanded = true;
    }
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    activity.SetTravel(ActivityTravel::None);
}

void EnterInside(ActivityServices& services, Activity& activity, DungeonRunState& state, Player* player)
{
    uint64 const now = services.nowMs;
    // Walked in while still at the finder: its queue, role check or proposal must not act on the run later (a
    // proposal accepted inside would unbind everyone). Nothing after the finder's own teleport (state Dungeon).
    DungeonLfg::Leave(activity.groupGuid);
    GoStep(activity, state, DungeonStep::Inside, now);
    FollowPlayer(services, activity, state, player);
    activity.SetCenter(ActivityPoint(), 0.0f);
    activity.SetPhase(ActivityPhase::Active, now);
    state.enteredMs = now;
    state.outsideSinceMs = 0;
    state.reachedDungeon = true;
}

void EnterIdle(ActivityServices& services, Activity& activity, DungeonRunState& state, ActivityOutcome outcome,
               char const* why)
{
    if (state.step == DungeonStep::Idle)
        return;
    uint64 const now = services.nowMs;
    DungeonLfg::Leave(activity.groupGuid);
    state.endOutcome = outcome;
    state.endWhy = Acore::StringFormat("{}: {}", activity.name, why);
    state.idleUntilMs = now + uint64(services.config.dungeonIdleDisbandMinutes) * MINUTE * IN_MILLISECONDS;
    GoStep(activity, state, DungeonStep::Idle, now);
    activity.SetTravel(ActivityTravel::None);
    activity.SetPhase(ActivityPhase::Resolving, now);
    if (state.reachedDungeon)
        LineDue(state, DungeonLine::Thanks, now);
}

// Bots FormGroup left out for the budget (only when MaxOpsPerTick is below the group's size).
void AddLate(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    if (state.toAdd.empty() || activity.groupGuid.IsEmpty())
        return;
    for (auto it = state.toAdd.begin(); it != state.toAdd.end();)
    {
        ActivityMember const* member = activity.FindMember(*it);
        if (!member || !member->In())
        {
            it = state.toAdd.erase(it);
            continue;
        }
        ActivityOpResult const result = services.ops.AddMember(activity.groupGuid, activity.leader, *it);
        if (result == ActivityOpResult::Deferred)
            break;
        ObjectGuid const guid = *it;
        it = state.toAdd.erase(it);
        if (result != ActivityOpResult::Done)
            activity.MarkLeft(guid, ActivityLeaveReason::Dropped, services.nowUnix);
    }
}

// Seats held for invitations that expired (the director dropped them from activity.invites unanswered).
void ReleaseHeld(Activity& activity, DungeonRunState& state)
{
    for (auto it = state.heldSeats.begin(); it != state.heldSeats.end();)
    {
        ActivityMember const* member = activity.FindMember(it->first);
        if (!activity.invites.count(it->first) && !(member && member->In()))
        {
            LOG_DEBUG(LOG, "activity #{} dungeon: the {} seat held for player {} is open again", activity.id,
                      RoleName(it->second), it->first.GetCounter());
            it = state.heldSeats.erase(it);
        }
        else
            ++it;
    }
}

// Real players who logged out in Recruit and kept their group slot: back in the world, members again with their
// role; out of the group, their seat opens again (Recruit) or is dropped (later steps).
void Rejoin(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    if (state.awayRoles.empty())
        return;
    Group* group = RunGroup(activity);
    for (auto it = state.awayRoles.begin(); it != state.awayRoles.end();)
    {
        ObjectGuid const guid = it->first;
        uint8 const role = it->second;
        if (group && group->IsMember(guid))
        {
            Player* player = InWorld(guid);
            if (!player)
            {
                ++it;
                continue;
            }
            // As ActivityDirector::Invitations enrolls a player (Enroll reopens its closed row).
            activity.Enroll(guid, 0, false, uint8(player->GetLevel()), services.nowUnix);
            ActivityStore::Instance().InsertMember(activity.id, guid.GetCounter(), false, ActivityRole::Member,
                                                   services.nowUnix);
            state.playerRoles[guid] = role;
            LOG_DEBUG(LOG, "activity #{} dungeon: player {} is back as {}", activity.id, guid.GetCounter(),
                      RoleName(role));
        }
        else
        {
            if (state.step == DungeonStep::Recruit)
                state.openRoles.push_back(role);
            LOG_DEBUG(LOG, "activity #{} dungeon: player {} left the group while away, the {} seat {}", activity.id,
                      guid.GetCounter(), RoleName(role), state.step == DungeonStep::Recruit ? "is open" : "is gone");
        }
        it = state.awayRoles.erase(it);
    }
}

// Prepare: queues and foreign instance binds dropped for the bots whose role fits (cheap, all at once), then one
// bot of another role gets a specialization of its seat's role per tick (gear and talents: expensive).
void Prepare(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    Player* anchor = InWorld(state.trigger);
    if (!anchor || anchor->GetLevel() < activity.minLevel || anchor->GetLevel() > activity.maxLevel)
        anchor = LeaderOf(activity);
    if (!anchor)
        return;  // the leader is gone: OnMemberLeft ends the run
    // Every bot is within [minLevel, maxLevel] and so is the anchor: this tolerance never rebuilds a level.
    uint32 const tolerance = uint32(activity.maxLevel - activity.minLevel);

    for (ObjectGuid const& guid : state.toClean)
    {
        ActivityMember const* member = activity.FindMember(guid);
        Player* bot = member && member->In() ? ActivityWorld::FindMember(*member) : nullptr;
        if (bot)
        {
            CoaLeaveBattlegroundQueues(bot);
            CoaDropForeignInstanceBinds(bot, anchor);
        }
    }
    state.toClean.clear();

    if (!state.toPrepare.empty())
    {
        ObjectGuid const guid = state.toPrepare.front();
        uint8 const role = state.toPrepareRole.empty() ? uint8(CoaRole::Dps) : state.toPrepareRole.front();
        state.toPrepare.erase(state.toPrepare.begin());
        if (!state.toPrepareRole.empty())
            state.toPrepareRole.erase(state.toPrepareRole.begin());

        ActivityMember const* member = activity.FindMember(guid);
        Player* bot = member && member->In() ? ActivityWorld::FindMember(*member) : nullptr;
        if (bot)
        {
            std::string why;
            if (DungeonLfg::PrepareRole(bot, anchor, role, tolerance, why))
            {
                LOG_DEBUG(LOG, "activity #{} dungeon: bot {} prepared as {}", activity.id, guid.GetCounter(),
                          RoleName(role));
                // Its combat strategies follow the new specialization.
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                    ai->SetCoaStrategiesDirty();
            }
            else
            {
                LOG_DEBUG(LOG, "activity #{} dungeon: bot {} not prepared as {}: {}", activity.id,
                          guid.GetCounter(), RoleName(role), why);
                activity.MarkLeft(guid, ActivityLeaveReason::Kicked, services.nowUnix);
            }
        }
    }

    if (activity.BotsIn() + 1 < state.seatRoles.size())
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: too few bots ready", activity.name));
        return;
    }
    if (state.toPrepare.empty())
        GoStep(activity, state, DungeonStep::Form, services.nowMs);
}

// Form: the leader's group with every bot at once when the budget allows it.
void Form(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    if (services.nowMs >= state.stepSinceMs + ActivityThreshold::FORM_TIMEOUT_MS)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: no group in time", activity.name));
        return;
    }

    std::vector<ObjectGuid> members;
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot && member.guid != activity.leader)
            members.push_back(member.guid);

    // The director's member pass drops a member without the activity's group once the group exists, before
    // AddMember could add it on a later tick: wait for a tick whose budget adds everyone (when MaxOpsPerTick can).
    uint32 const cost = 1 + uint32(members.size());
    uint32 const budget = services.config.maxOpsPerTick;
    if (cost <= budget && services.ops.Used() + cost > budget)
        return;

    ObjectGuid groupGuid;
    std::vector<ObjectGuid> added;
    std::vector<ObjectGuid> skipped;
    ActivityOpResult const result = services.ops.FormGroup(activity.leader, members, uint8(GROUP_LOOT),
                                                           uint8(ITEM_QUALITY_UNCOMMON), groupGuid, added, skipped);
    if (result == ActivityOpResult::Deferred)
        return;
    if (result != ActivityOpResult::Done || groupGuid.IsEmpty())
    {
        activity.Finish(ActivityOutcome::TooFew,
                        Acore::StringFormat("{}: group not formed ({})", activity.name, ActivityOpResultName(result)));
        return;
    }

    activity.groupGuid = groupGuid;
    activity.viewDirty = true;
    state.toAdd = std::move(skipped);
    LOG_DEBUG(LOG, "activity #{} dungeon: group {} formed, {} added, {} left for later", activity.id,
              groupGuid.GetCounter(), added.size(), state.toAdd.size());
    EnterRecruit(services, activity, state);
}

// The trigger player hears about the run from the leader (once, after the first post): online, ungrouped, its role
// still open, and no opt-out of unasked whispers.
void Offer(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    Player* trigger = InWorld(state.trigger);
    CoaPlayerPrefs const& prefs = CoaPlayerPrefs::Instance();
    if (!trigger || !Takes(activity, state, trigger) || !prefs.IsLoaded())
    {
        state.offerSent = true;
        return;
    }
    CoaPrefs const playerPrefs = prefs.Get(trigger->GetSession()->GetAccountId());
    if (playerPrefs.VoiceOptedOut() || (playerPrefs.activityMask & COA_OPTOUT_WHISPERS))
    {
        state.offerSent = true;
        return;
    }

    std::string const text = Spoken(ActivityWorld::Line(
        "coa_act_recruit_offer", "We have a group for %target and room for you. Whisper me inv.",
        {{"%target", activity.name}}));
    ActivityOpResult const result =
        services.ops.PostToChannel(activity.leader, ActivityChatTarget::Whisper, text, state.trigger);
    if (result == ActivityOpResult::Deferred)
        return;
    state.offerSent = true;
    LOG_DEBUG(LOG, "activity #{} dungeon: offer to player {} {}", activity.id, state.trigger.GetCounter(),
              ActivityOpResultName(result));
}

// Recruit: the LFM in the recruitment channels (Chat.Channel), its text name from the brain or the dice.
void Post(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    ActivityConfig const& config = services.config;
    uint64 const now = services.nowMs;
    if (now < state.nextPostMs || config.chatChannel == ActivityChatChannel::None || !activity.CanPost(config, now))
        return;
    std::vector<uint8> const open = UnheldOpen(state);
    if (open.empty() || !HasRoom(activity, state))
        return;  // every open seat waits for an answer to an invitation (or a player who logged out)

    if (state.lineKey.empty())
    {
        ActivityOrganizer& organizer = services.organizer;
        ActivityAskState const ask = organizer.State(activity.id, ActivityAsk::LineKey);
        if (ask == ActivityAskState::Pending)
            return;
        std::size_t index = 0;
        if (ask == ActivityAskState::Ready)
        {
            std::optional<BrainResult> const result = organizer.Take(activity.id, ActivityAsk::LineKey);
            std::optional<std::size_t> const pick =
                result ? ActivityBrain::ReadLine(*result, std::size(LFM_KEYS)) : std::nullopt;
            index = pick ? *pick : std::size_t(urand(0, 1));
        }
        else
        {
            if (organizer.Usable())
            {
                Player* leader = LeaderOf(activity);
                uint32 const zoneId = leader ? leader->GetZoneId() : activity.center.zoneId;
                BrainJob job = ActivityBrain::BuildLineJob(
                    ActivityKindId::Dungeon, EnglishZoneName(zoneId), {LFM_KEYS[0], LFM_KEYS[1]},
                    {LFM_ENGLISH[0], LFM_ENGLISH[1]}, 0, 0);
                if (organizer.Ask(activity.id, ActivityAsk::LineKey, std::move(job)))
                    return;
            }
            index = std::size_t(urand(0, 1));
        }
        state.lineKey = LFM_KEYS[std::min<std::size_t>(index, 1)];
    }
    std::size_t const key = state.lineKey == LFM_KEYS[1] ? 1 : 0;

    std::string const text = Spoken(ActivityWorld::Line(
        LFM_KEYS[key], LFM_ENGLISH[key],
        {{"%dungeon", activity.name},
         {"%role", RoleWords(open)},
         {"%count", Acore::StringFormat("{}/{}", activity.BotsIn() + activity.PlayersIn(), PARTY_SIZE)}}));
    ActivityOpResult const result =
        config.chatChannel == ActivityChatChannel::World
            ? services.ops.PostToChannel(activity.leader, ActivityChatTarget::World, text)
            : services.ops.PostToChannels(activity.leader, config.chatChannels, text);
    if (result == ActivityOpResult::Deferred)
        return;
    state.postTried = true;
    if (result == ActivityOpResult::Done)
    {
        activity.NotePost(now);
        state.nextPostMs =
            now + uint64(std::max(config.chatMinGapSeconds, config.chatRecruitEverySeconds)) * IN_MILLISECONDS;
        state.lineKey.clear();
    }
    else
        state.nextPostMs = now + POST_RETRY_MS;
    LOG_DEBUG(LOG, "activity #{} dungeon: LFM {}, open {}", activity.id, ActivityOpResultName(result),
              RoleNames(open));
}

void Recruit(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    AddLate(services, activity, state);
    ReleaseHeld(activity, state);
    if (activity.finishing)
        return;
    // A seat kept for a player who logged out waits for it until the recruitment ends.
    if (state.openRoles.empty() && state.awayRoles.empty())
    {
        EnterQueue(services, activity, state);
        return;
    }
    if (services.nowMs >= state.recruitUntilMs)
    {
        if (activity.PlayersIn() > 0)
            EnterQueue(services, activity, state);
        else
            activity.Finish(ActivityOutcome::TooFew,
                            Acore::StringFormat("{}: {}", activity.name,
                                                state.awayRoles.empty() ? "nobody answered the LFM"
                                                                        : "the player did not come back"));
        return;
    }
    Post(services, activity, state);
    if (state.postTried && !state.offerSent)
        Offer(services, activity, state);
}

// Queue: the leader queues the group at the Dungeon Finder (the players accept the role check and the proposal in
// their own UI; the finder sends everyone in). Dungeon.LfgTries attempts, then the walk to the portal.
void Queue(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    ActivityConfig const& config = services.config;
    uint64 const now = services.nowMs;
    AddLate(services, activity, state);
    if (activity.finishing)
        return;

    // The finder's teleport may have happened between two ticks.
    if (Player* inside = RealMemberOn(activity, state.dungeon.mapId))
    {
        EnterInside(services, activity, state, inside);
        return;
    }

    if (!config.dungeonUseLfg || !DungeonLfg::FinderEnabled())
    {
        DungeonLfg::Leave(activity.groupGuid);
        WalkOrFinish(services, activity, state, "no Dungeon Finder and no portal");
        return;
    }

    uint64 const queueMs = uint64(config.dungeonQueueMinutes) * MINUTE * IN_MILLISECONDS;
    DungeonQueueState const finder = DungeonLfg::State(activity.groupGuid);
    switch (finder)
    {
        case DungeonQueueState::Dungeon:
        case DungeonQueueState::Finished:
            // The proposal passed: everyone is being sent in. Waited for Dungeon.QueueMinutes at most.
            if (!state.dungeonSeenMs)
                state.dungeonSeenMs = now;
            else if (now >= state.dungeonSeenMs + queueMs)
                EnterIdle(services, activity, state, ActivityOutcome::TooFew, "the player did not enter the dungeon");
            return;
        case DungeonQueueState::RoleCheck:
        case DungeonQueueState::Queued:
        case DungeonQueueState::Proposal:
            state.lfgPending = true;
            if (now >= state.queueUntilMs)
            {
                LOG_DEBUG(LOG, "activity #{} dungeon: no group from the Dungeon Finder in time ({})", activity.id,
                          DungeonQueueStateName(finder));
                DungeonLfg::Leave(activity.groupGuid);
                state.lfgPending = false;
                state.retryAtMs = now + QUEUE_RETRY_MS;
            }
            return;
        case DungeonQueueState::None:
            break;
        default:  // Boot, raid browser: not expected for this group; the hard deadline covers it
            return;
    }

    if (state.lfgPending)
    {
        // Back to None after an attempt: a role check refused, a proposal declined or failed.
        LOG_DEBUG(LOG, "activity #{} dungeon: the Dungeon Finder dropped the group (attempt {})", activity.id,
                  state.lfgTries);
        state.lfgPending = false;
        state.retryAtMs = now + QUEUE_RETRY_MS;
    }
    if (now < state.retryAtMs)
        return;
    if (state.lfgTries >= config.dungeonLfgTries)
    {
        WalkOrFinish(services, activity, state, "the Dungeon Finder did not form the group");
        return;
    }
    // A pending invitation or a bot not added yet would change the group under the finder: wait for them.
    if (!state.toAdd.empty() || !activity.invites.empty())
        return;
    // Only a full group keeps its guid through the finder (LFGMgr::MakeNewGroup converts it in place); a smaller one
    // would be dissolved into a new group with strangers: it walks instead, no attempt spent.
    Group* group = RunGroup(activity);
    if (!group || group->GetMembersCount() < PARTY_SIZE)
    {
        LOG_DEBUG(LOG, "activity #{} dungeon: {} members, not queued", activity.id,
                  group ? group->GetMembersCount() : 0);
        WalkOrFinish(services, activity, state, "the group is not full");
        return;
    }

    ++state.lfgTries;
    std::string why;
    if (DungeonLfg::Queue(activity.leader, activity.groupGuid, state.dungeon.id, why))
    {
        state.queueUntilMs = now + queueMs;
        state.lfgPending = true;
        LOG_DEBUG(LOG, "activity #{} dungeon: queued for {} (attempt {}/{})", activity.id, state.dungeon.englishName,
                  state.lfgTries, config.dungeonLfgTries);
        if (!state.queueLineSaid)
        {
            state.queueLineSaid = true;
            LineDue(state, DungeonLine::Queue, now);
        }
    }
    else
    {
        LOG_DEBUG(LOG, "activity #{} dungeon: not queued (attempt {}/{}): {}", activity.id, state.lfgTries,
                  config.dungeonLfgTries, why);
        state.retryAtMs = now + QUEUE_RETRY_MS;
    }
}

void Walk(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    if (Player* inside = RealMemberOn(activity, state.dungeon.mapId))
    {
        EnterInside(services, activity, state, inside);
        return;
    }
    Player* leader = LeaderOf(activity);
    ActivityPoint const& portal = state.dungeon.portal;
    if (leader && leader->GetMapId() == portal.mapId &&
        portal.SqDist2d(leader->GetPositionX(), leader->GetPositionY()) <= PORTAL_ARRIVE_YARDS * PORTAL_ARRIVE_YARDS)
        EnterPortal(services, activity, state);
}

void AtPortal(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    if (Player* inside = RealMemberOn(activity, state.dungeon.mapId))
    {
        EnterInside(services, activity, state, inside);
        return;
    }
    if (services.nowMs >= state.stepSinceMs + uint64(services.config.dungeonQueueMinutes) * MINUTE * IN_MILLISECONDS)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: the player did not come", activity.name));
        return;
    }
    if (!activity.follow.IsEmpty())
        return;

    // A real member at the portal: the bots follow it in.
    ActivityPoint const& portal = state.dungeon.portal;
    float const reachSq = PORTAL_PLAYER_YARDS * PORTAL_PLAYER_YARDS;
    for (ActivityMember const& member : activity.members)
    {
        if (member.isBot || !member.In())
            continue;
        Player* player = ActivityWorld::FindMember(member);
        if (player && player->GetMapId() == portal.mapId &&
            portal.SqDist2d(player->GetPositionX(), player->GetPositionY()) <= reachSq)
        {
            LOG_DEBUG(LOG, "activity #{} dungeon: player {} at the portal", activity.id, member.guid.GetCounter());
            FollowPlayer(services, activity, state, player);
            return;
        }
    }
}

void Inside(ActivityServices& services, Activity& activity, DungeonRunState& state)
{
    uint64 const now = services.nowMs;
    if (DungeonLfg::State(activity.groupGuid) == DungeonQueueState::Finished)
    {
        state.finished = true;
        EnterIdle(services, activity, state, ActivityOutcome::Success, "finished");
        return;
    }

    Player* followed = activity.FollowedPlayer();
    Player* inside = followed && followed->GetMapId() == state.dungeon.mapId
                         ? followed
                         : RealMemberOn(activity, state.dungeon.mapId);
    if (!inside)
    {
        if (!state.outsideSinceMs)
            state.outsideSinceMs = now;
        else if (now >= state.outsideSinceMs + OUTSIDE_END_MS)
            EnterIdle(services, activity, state, ActivityOutcome::Success, "left the dungeon");
        return;
    }
    state.outsideSinceMs = 0;
    if (!followed)
    {
        // The followed player left the group or logged out: the bots follow the one inside now.
        state.leadHanded = false;
        FollowPlayer(services, activity, state, inside);
        followed = inside;
    }
    if (followed != inside || now < state.enteredMs + ENTER_GRACE_MS)
        return;

    // Bots still outside the followed player's instance: to the entrance, under the teleport's visibility rule.
    for (ActivityMember const& member : activity.members)
    {
        if (!member.isBot || !member.In())
            continue;
        Player* bot = ActivityWorld::FindMember(member);
        if (!bot || (bot->GetMapId() == followed->GetMapId() && bot->GetInstanceId() == followed->GetInstanceId()))
            continue;
        auto const last = state.enterTeleportMs.find(member.guid);
        if (last != state.enterTeleportMs.end() && now < last->second + ENTER_RETRY_MS)
            continue;
        ActivityOpResult const result = services.ops.Teleport(member.guid, state.dungeon.entrance);
        if (result == ActivityOpResult::Deferred)
            break;
        state.enterTeleportMs[member.guid] = now;
        LOG_DEBUG(LOG, "activity #{} dungeon: bot {} to the entrance {}", activity.id, member.guid.GetCounter(),
                  ActivityOpResultName(result));
    }
}
}  // namespace

char const* DungeonStepName(DungeonStep step)
{
    switch (step)
    {
        case DungeonStep::Prepare:
            return "prepare";
        case DungeonStep::Form:
            return "form";
        case DungeonStep::Recruit:
            return "recruit";
        case DungeonStep::Queue:
            return "queue";
        case DungeonStep::Walk:
            return "walk";
        case DungeonStep::Portal:
            return "portal";
        case DungeonStep::Inside:
            return "inside";
        case DungeonStep::Idle:
            return "idle";
        default:
            return "unknown";
    }
}

bool DungeonRunKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                          std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    ActivityConfig const& config = services.config;
    uint64 const now = services.nowMs;
    bool const ambient = !request.gm;

    for (auto it = playerCooldownUntilMs.begin(); it != playerCooldownUntilMs.end();)
    {
        if (it->second <= now)
            it = playerCooldownUntilMs.erase(it);
        else
            ++it;
    }
    auto const unfit = [&](Player* candidate) -> char const*
    {
        if (char const* reason = PlayerUnfit(candidate, request))
            return reason;
        if (ambient && playerCooldownUntilMs.count(candidate->GetGUID().GetCounter()))
            return "planned for lately";
        return nullptr;
    };

    // 1. The player the run is for: the one it was placed near, else (ambient) any fitting one.
    Player* player = nullptr;
    if (request.nearPlayer)
    {
        char const* const reason = unfit(request.nearPlayer);
        if (!reason)
            player = request.nearPlayer;
        else if (!ambient)
        {
            why = Acore::StringFormat("the player is {}", reason);
            return false;
        }
    }
    else if (!ambient)
    {
        why = "no player to run it for: start it from a character in the world";
        return false;
    }
    if (!player && !services.realPlayers.empty())
    {
        std::size_t const count = services.realPlayers.size();
        std::size_t const start = urand(0, uint32(count) - 1);
        for (std::size_t k = 0; k < count && !player; ++k)
        {
            Player* const candidate = services.realPlayers[(start + k) % count];
            if (!unfit(candidate))
                player = candidate;
        }
    }
    if (!player)
    {
        why = "no ungrouped, unqueued player out in the world";
        return false;
    }

    // 2. The dungeon and the bots' level window.
    uint8 const team = uint8(player->GetTeamId());
    uint8 const level = request.level ? request.level : uint8(player->GetLevel());
    bool const walkOnly = !config.dungeonUseLfg || !DungeonLfg::FinderEnabled();
    std::optional<DungeonInfo> const dungeon = walkOnly
                                                   ? PickWalkable(config.dungeonIds, level, team, player->GetMapId())
                                                   : DungeonLfg::Pick(config.dungeonIds, level, level, level);
    if (!dungeon)
    {
        bool const anyDungeon = walkOnly && DungeonLfg::Pick(config.dungeonIds, level, level, level);
        why = anyDungeon ? Acore::StringFormat("no dungeon with a known portal for level {}", uint32(level))
                         : Acore::StringFormat("no dungeon for level {}", uint32(level));
        return false;
    }
    int32 const spread = int32(config.dungeonLevelSpread);
    uint8 const minLevel = uint8(std::max(int32(level) - spread, int32(dungeon->minLevel)));
    uint8 const maxLevel = uint8(std::min(int32(level) + spread, int32(dungeon->maxLevel)));

    // 3. Seats: the five roles minus the player's; 3-4 bots, the rest open to real players (the player's first).
    uint8 const playerRole = DungeonLfg::RoleOf(player);
    std::vector<uint8> roles{uint8(CoaRole::Tank), uint8(CoaRole::Heal), uint8(CoaRole::Dps), uint8(CoaRole::Dps),
                             uint8(CoaRole::Dps)};
    auto const own = std::find(roles.begin(), roles.end(), playerRole);
    if (own != roles.end())
        roles.erase(own);
    uint32 bots = std::clamp<uint32>(request.size ? request.size : config.dungeonBots, 3, 4);
    bots = std::min(bots, request.capacity);
    if (bots < 3)
    {
        why = Acore::StringFormat("no participant place ({} left, a run needs 3)", request.capacity);
        return false;
    }

    auto state = std::make_unique<DungeonRunState>();
    state->dungeon = *dungeon;
    state->trigger = player->GetGUID();
    state->seatRoles.assign(roles.begin(), roles.begin() + bots);
    state->openRoles.push_back(playerRole);
    state->openRoles.insert(state->openRoles.end(), roles.begin() + bots, roles.end());

    // 4. Candidates by seat (FindCoaRecruit: same faction, on the player's map first, of the role first).
    ActivityPoint const rally = ActivityWorld::PointOf(player);
    std::set<ObjectGuid> skip;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (!bot || !bot->IsInWorld())
            continue;
        if (uint8(bot->GetTeamId()) != team || bot->GetLevel() < minLevel || bot->GetLevel() > maxLevel)
            skip.insert(bot->GetGUID());
    }

    plan.candidates.clear();
    for (std::size_t seat = 0; seat < state->seatRoles.size(); ++seat)
    {
        uint8 const role = state->seatRoles[seat];
        std::size_t const quota = seat == 0 ? 1 : 2;
        std::vector<Player*> fitting;
        std::vector<Player*> fallback;
        std::vector<Player*> unused;
        for (uint32 tries = 0; tries < SEAT_TRIES; ++tries)
        {
            // Seat 0 leads and posts in its zone's channels: it looks further for one in the player's zone.
            if (seat != 0 && fitting.size() >= quota)
                break;
            if (seat == 0 && !fitting.empty() && fitting.back()->GetMapId() == player->GetMapId() &&
                fitting.back()->GetZoneId() == player->GetZoneId())
                break;
            bool fits = false;
            Player* const bot = FindCoaRecruit(player, CoaRole(role), 0, skip, fits);
            if (!bot)
                break;
            skip.insert(bot->GetGUID());
            if (!services.candidates.IsAvailable(bot) || uint8(bot->GetTeamId()) != team ||
                bot->GetLevel() < minLevel || bot->GetLevel() > maxLevel)
                continue;
            if (fits)
                fitting.push_back(bot);
            else if (config.dungeonSwitchSpecs)
                fallback.push_back(bot);
            else
                unused.push_back(bot);
        }

        std::vector<std::pair<Player*, bool>> taken;
        if (seat == 0)
        {
            if (Player* const leader = BestLeader(fitting, player))
                taken.emplace_back(leader, true);
            else if (Player* const other = BestLeader(fallback, player))
                taken.emplace_back(other, false);
        }
        else
        {
            for (Player* const bot : fitting)
                if (taken.size() < quota)
                    taken.emplace_back(bot, true);
            for (Player* const bot : fallback)
                if (taken.size() < quota)
                    taken.emplace_back(bot, false);
        }
        if (taken.empty())
        {
            why = Acore::StringFormat("no free {} of levels {}-{}", RoleName(role), uint32(minLevel),
                                      uint32(maxLevel));
            return false;
        }

        // The free bots not taken go back to the pool: another seat's role may fit them.
        auto const isTaken = [&taken](Player* bot)
        {
            return std::any_of(taken.begin(), taken.end(),
                               [bot](std::pair<Player*, bool> const& entry) { return entry.first == bot; });
        };
        for (std::vector<Player*> const* list : {&fitting, &fallback, &unused})
            for (Player* const bot : *list)
                if (!isTaken(bot))
                    skip.erase(bot->GetGUID());

        for (auto const& [bot, fits] : taken)
        {
            plan.candidates.push_back(ActivityCandidates::Describe(bot, rally));
            state->candidateRole.push_back(role);
            state->candidateSeat.push_back(uint8(seat));
            state->candidateFits.push_back(fits);
        }
    }

    // 5. The plan.
    plan.kind = ActivityKindId::Dungeon;
    plan.gm = request.gm;
    plan.team = team;
    plan.minLevel = minLevel;
    plan.maxLevel = maxLevel;
    plan.rally = rally;
    plan.target = Acore::StringFormat("{} {}", dungeon->id, dungeon->englishName);
    plan.name = dungeon->name.empty() ? dungeon->englishName : dungeon->name;
    plan.wanted = bots;
    plan.needed = bots;
    plan.leaderFixed = true;
    plan.decisions.clear();

    LOG_DEBUG(LOG, "dungeon plan: player {} ({}) level {} {}, {} ({}) levels {}-{}, seats {}, open {}, {} candidates{}",
              player->GetName(), player->GetGUID().GetCounter(), uint32(player->GetLevel()), TeamName(team),
              dungeon->englishName, dungeon->id, uint32(minLevel), uint32(maxLevel), RoleNames(state->seatRoles),
              RoleNames(state->openRoles), plan.candidates.size(), walkOnly ? ", walking" : "");
    plan.state = std::move(state);
    return true;
}

bool DungeonRunKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    DungeonRunState* state = dynamic_cast<DungeonRunState*>(plan.state.get());
    if (!state || state->seatRoles.empty() || plan.candidates.empty() ||
        state->candidateSeat.size() != plan.candidates.size() || plan.decisions.size() != plan.candidates.size())
        return false;

    // Each seat: its first candidate that said yes.
    std::vector<std::size_t> chosen(state->seatRoles.size(), plan.candidates.size());
    for (std::size_t i = 0; i < plan.candidates.size(); ++i)
    {
        std::size_t const seat = state->candidateSeat[i];
        if (seat < chosen.size() && chosen[seat] == plan.candidates.size() && plan.decisions[i] == 1)
            chosen[seat] = i;
    }
    for (std::size_t seat = 0; seat < chosen.size(); ++seat)
    {
        if (chosen[seat] == plan.candidates.size())
        {
            LOG_DEBUG(LOG, "dungeon plan: nobody for the {} seat", RoleName(state->seatRoles[seat]));
            return false;
        }
    }

    uint64 const now = services.nowMs;
    activity.leader = plan.candidates[chosen[0]].guid;
    activity.name = plan.name;
    activity.target = plan.target;
    activity.minLevel = plan.minLevel;
    activity.maxLevel = plan.maxLevel;
    state->seatBots.clear();
    for (std::size_t seat = 0; seat < chosen.size(); ++seat)
    {
        std::size_t const i = chosen[seat];
        ActivityCandidate const& candidate = plan.candidates[i];
        activity.Enroll(candidate.guid, candidate.epoch, true, candidate.level, services.nowUnix);
        state->seatBots.push_back(candidate.guid);
        if (state->candidateFits[i])
            state->toClean.push_back(candidate.guid);
        else
        {
            state->toPrepare.push_back(candidate.guid);
            state->toPrepareRole.push_back(state->seatRoles[seat]);
        }
    }

    ActivityConfig const& config = services.config;
    activity.hardDeadlineMs =
        now + uint64(config.dungeonRecruitMinutes + config.dungeonQueueMinutes + config.dungeonMaxMinutes +
                     ActivityThreshold::HARD_DEADLINE_EXTRA_MIN) *
                  MINUTE * IN_MILLISECONDS;
    playerCooldownUntilMs[state->trigger.GetCounter()] =
        now + uint64(config.dungeonPlayerCooldownMinutes) * MINUTE * IN_MILLISECONDS;

    state->step = DungeonStep::Prepare;
    state->stepSinceMs = now;
    activity.state = std::move(plan.state);
    activity.SetCenter(plan.rally, CENTER_RADIUS);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Forming, now);
    return true;
}

void DungeonRunKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;
    DungeonRunState* state = StateOf(activity);
    if (!state)
    {
        activity.Finish(ActivityOutcome::TooFew, "dungeon run without its state");
        return;
    }
    if (activity.BotsIn() == 0)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: no bot left", activity.name));
        return;
    }

    if (state->step >= DungeonStep::Recruit && state->step < DungeonStep::Idle)
        Rejoin(services, activity, *state);

    switch (state->step)
    {
        case DungeonStep::Prepare:
            Prepare(services, activity, *state);
            break;
        case DungeonStep::Form:
            Form(services, activity, *state);
            break;
        case DungeonStep::Recruit:
            Recruit(services, activity, *state);
            break;
        case DungeonStep::Queue:
            Queue(services, activity, *state);
            break;
        case DungeonStep::Walk:
            Walk(services, activity, *state);
            break;
        case DungeonStep::Portal:
            AtPortal(services, activity, *state);
            break;
        case DungeonStep::Inside:
            Inside(services, activity, *state);
            break;
        case DungeonStep::Idle:
            if (services.nowMs >= state->idleUntilMs)
            {
                activity.Finish(state->endOutcome, state->endWhy);
                return;
            }
            break;
    }

    if (activity.finishing)
        return;
    SayDue(services, activity, *state);
    TellMeet(services, activity, *state);
}

void DungeonRunKind::OnMemberLeft(ActivityServices& services, Activity& activity, ActivityMember const& member)
{
    DungeonRunState* state = StateOf(activity);
    if (!state)
        return;
    auto const drop = [&member](std::vector<ObjectGuid>& list)
    { list.erase(std::remove(list.begin(), list.end(), member.guid), list.end()); };
    drop(state->toAdd);
    drop(state->toClean);
    for (std::size_t i = 0; i < state->toPrepare.size();)
    {
        if (state->toPrepare[i] != member.guid)
        {
            ++i;
            continue;
        }
        state->toPrepare.erase(state->toPrepare.begin() + std::ptrdiff_t(i));
        if (i < state->toPrepareRole.size())
            state->toPrepareRole.erase(state->toPrepareRole.begin() + std::ptrdiff_t(i));
    }
    state->heldSeats.erase(member.guid);
    if (state->step == DungeonStep::Idle || activity.finishing)
        return;

    if (!member.isBot)
    {
        auto const filled = state->playerRoles.find(member.guid);
        if (state->step == DungeonStep::Recruit)
        {
            if (filled != state->playerRoles.end())
            {
                // Logged out, still in the group: its slot stays taken, so its seat waits for it (Rejoin) instead
                // of being advertised. Otherwise its seat is open again.
                Group* group = RunGroup(activity);
                if (member.leftReason == ActivityLeaveReason::Gone && group && group->IsMember(member.guid))
                    state->awayRoles[member.guid] = filled->second;
                else
                    state->openRoles.push_back(filled->second);
            }
        }
        else if (state->step > DungeonStep::Recruit && activity.PlayersIn() == 0)
            EnterIdle(services, activity, *state, state->reachedDungeon ? ActivityOutcome::Success
                                                                        : ActivityOutcome::TooFew,
                      "the player left");
        if (filled != state->playerRoles.end())
            state->playerRoles.erase(filled);
        return;
    }

    // Once inside the player leads and the director's member pass looks after the rest.
    if (state->step >= DungeonStep::Inside)
        return;
    if (member.guid == activity.leader)
    {
        activity.Finish(ActivityOutcome::LeaderGone, Acore::StringFormat("{}: the leader left ({})", activity.name,
                                                                         ActivityLeaveReasonName(member.leftReason)));
        return;
    }
    if (activity.BotsIn() + 1 < state->seatRoles.size())
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: too few bots left", activity.name));
        return;
    }
    // Before the queue its seat opens to real players (the LFM asks for it).
    if (state->step <= DungeonStep::Recruit)
    {
        for (std::size_t seat = 0; seat < state->seatBots.size() && seat < state->seatRoles.size(); ++seat)
        {
            if (state->seatBots[seat] != member.guid)
                continue;
            state->openRoles.push_back(state->seatRoles[seat]);
            LOG_DEBUG(LOG, "activity #{} dungeon: the {} seat of bot {} is open to players", activity.id,
                      RoleName(state->seatRoles[seat]), member.guid.GetCounter());
            break;
        }
    }
}

std::string DungeonRunKind::Join(ActivityServices& services, Activity& activity, Player* player)
{
    COA_ASSERT_WORLD_THREAD();
    std::string const none = ActivityWorld::Line("coa_act_join_none", "No bot activity you can join right now.");
    DungeonRunState* state = StateOf(activity);
    if (!player || !state)
        return none;
    std::string const closed = ActivityWorld::Line("coa_act_join_closed", "That group is not recruiting any more.");
    if (state->step != DungeonStep::Recruit || activity.groupGuid.IsEmpty() || activity.finishing)
        return closed;
    Player* leader = LeaderOf(activity);
    if (!leader)
        return none;

    ObjectGuid const guid = player->GetGUID();
    auto const inviteLine = [&]()
    {
        return ActivityWorld::Line("coa_act_join_group",
                                   "%leader invites you to the group for %target: accept the invitation.",
                                   {{"%leader", ActivityWorld::FirstName(leader)}, {"%target", activity.name}});
    };
    // Asked again while the invitation is out: the same answer.
    if (state->heldSeats.count(guid) && activity.invites.count(guid))
        return inviteLine();

    std::vector<uint8> const open = UnheldOpen(*state);
    if (open.empty() || !HasRoom(activity, *state))
        return closed;
    uint8 const role = DungeonLfg::RoleOf(player);
    if (std::find(open.begin(), open.end(), role) == open.end())
        return ActivityWorld::Line("coa_act_join_role", "We already have that role, we need %role.",
                                   {{"%role", RoleWords(open)}});
    DungeonInfo const& dungeon = state->dungeon;
    if (player->GetLevel() < dungeon.minLevel || player->GetLevel() > dungeon.maxLevel)
        return ActivityWorld::Line("coa_act_join_level", "That activity is for levels %min to %max.",
                                   {{"%min", std::to_string(uint32(dungeon.minLevel))},
                                    {"%max", std::to_string(uint32(dungeon.maxLevel))}});

    ActivityOpResult const result = services.ops.InvitePlayer(activity.leader, guid);
    LOG_DEBUG(LOG, "activity #{} dungeon: invite of {} as {} {}", activity.id, guid.GetCounter(), RoleName(role),
              ActivityOpResultName(result));
    if (result == ActivityOpResult::Refused)
        return ActivityWorld::Line("coa_act_join_grouped", "Leave your group first.");
    if (result != ActivityOpResult::Done)
        return none;

    activity.invites[guid] = services.nowMs + ActivityThreshold::INVITE_TIMEOUT_MS;
    state->heldSeats[guid] = role;
    return inviteLine();
}

bool DungeonRunKind::Accepts(ActivityServices& /*services*/, Activity const& activity, Player* player)
{
    DungeonRunState const* state = StateOf(activity);
    return state && Takes(activity, *state, player);
}

bool DungeonRunKind::OnPlayerJoined(ActivityServices& /*services*/, Activity& activity, ActivityMember const& member)
{
    DungeonRunState* state = StateOf(activity);
    if (!state)
        return false;
    uint8 role = uint8(CoaRole::Dps);
    auto const held = state->heldSeats.find(member.guid);
    if (held != state->heldSeats.end())
    {
        role = held->second;
        state->heldSeats.erase(held);
    }
    else if (Player* player = ActivityWorld::FindMember(member))
        role = DungeonLfg::RoleOf(player);

    auto const open = std::find(state->openRoles.begin(), state->openRoles.end(), role);
    if (open != state->openRoles.end())
        state->openRoles.erase(open);
    state->playerRoles[member.guid] = role;
    LOG_DEBUG(LOG, "activity #{} dungeon: player {} joined as {}, open {}", activity.id, member.guid.GetCounter(),
              RoleName(role), RoleNames(state->openRoles));
    return true;
}

void DungeonRunKind::OnEnd(ActivityServices& /*services*/, Activity& activity)
{
    DungeonLfg::Leave(activity.groupGuid);
    DungeonRunState* state = StateOf(activity);
    if (!state)
        return;
    LOG_DEBUG(LOG, "activity #{} dungeon: ends at step {} ({} finder attempts, {})", activity.id,
              DungeonStepName(state->step), state->lfgTries, state->reachedDungeon ? "was inside" : "never inside");
    state->toAdd.clear();
    state->heldSeats.clear();
    state->awayRoles.clear();
    state->lineDue = uint8(DungeonLine::None);
}

std::string DungeonRunKind::Describe(Activity const& activity, uint64 nowMs) const
{
    DungeonRunState const* state = StateOf(activity);
    if (!state)
        return "dungeon run without its state";
    DungeonInfo const& dungeon = state->dungeon;
    uint64 const inStepS = nowMs > state->stepSinceMs ? (nowMs - state->stepSinceMs) / IN_MILLISECONDS : 0;
    return Acore::StringFormat(
        "dungeon {} (id {}, map {}, levels {}-{}), step {} for {} s, finder {}, attempts {}/{}, open {}, invited {}, "
        "away {}, portal {}, {}",
        dungeon.englishName, dungeon.id, dungeon.mapId, uint32(dungeon.minLevel), uint32(dungeon.maxLevel),
        DungeonStepName(state->step), inStepS, DungeonQueueStateName(DungeonLfg::State(activity.groupGuid)),
        state->lfgTries, ActivityConfig::Current()->dungeonLfgTries, RoleNames(state->openRoles),
        state->heldSeats.size(), state->awayRoles.size(), PortalUsable(dungeon, activity.team) ? "known" : "none",
        state->reachedDungeon ? (state->finished ? "finished" : "inside once") : "not inside yet");
}

std::unique_ptr<ActivityKind> CreateDungeonRunKind() { return std::make_unique<DungeonRunKind>(); }
