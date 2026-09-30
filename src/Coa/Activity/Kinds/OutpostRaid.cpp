/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "OutpostRaid.h"
#include "ActivityBrain.h"
#include "ActivityCensus.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "ActivityOrganizer.h"
#include "CoaThread.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_set>

namespace
{
constexpr char const* LOG = "playerbots.activity";
constexpr uint64 RETREAT_ASK_MS = 15000;         // Active: the retreat question (or the dice) this often
constexpr uint32 RETREAT_ALIVE_PERCENT = 40;     // dice: fewer bots alive than this share of the bots in: fall back
constexpr uint64 REGROUP_WAIT_MS = 90000;        // a fall-back regroups at the rally this long at most
constexpr uint64 FORMING_MS = 30000;             // Forming: late bots are added this long at most
constexpr float ENGAGE_YARDS = 60.0f;            // Marching ends with the leader this close to the target
constexpr uint64 DEFENDER_HOLD_MS = 300000;      // a defender near the target this long ends the raid
constexpr uint64 DISBAND_STEP_MS = 4000;         // Disbanding: one bot leaves the group this often
constexpr uint64 LINE_RETRY_MS = 15000;          // a leader's line is retried this long (budget, combat)
constexpr uint32 LEADER_POOL = 10;               // free bots near the player looked at for the leader
constexpr float LEADER_NEAR_YARDS = 1500.0f;     // the leader is found this close to the player (its zone channel)
constexpr float FORMING_RADIUS = 30.0f;          // Recruiting and Forming: the center is the leader's spot
constexpr float RALLY_DIRECTION_YARDS = 100.0f;  // a rally without coordinates: toward a random point this far
constexpr int RALLY_TRIES = 4;
constexpr float CENTER_FOLLOW_YARDS = 15.0f;  // Marching/Active: the center follows the target once it moved this far
constexpr uint32 JOIN_LEVEL_MARGIN = 5;
constexpr uint64 POST_RETRY_MS = 60000;  // a refused or failed recruitment post is tried again this much later
constexpr std::size_t MAX_TARGETS = 4;
constexpr uint8 TOP_LEVEL = 80;
constexpr float TWO_PI = 6.28318530718f;

std::array<uint32, 6> const SUCCESS_EMOTES = {TEXT_EMOTE_CHEER, TEXT_EMOTE_ROAR,    TEXT_EMOTE_FLEX,
                                              TEXT_EMOTE_DANCE, TEXT_EMOTE_VICTORY, TEXT_EMOTE_APPLAUD};
std::array<char const*, 2> const LFM_KEYS = {"coa_act_lfm_raid", "coa_act_lfm_raid_alt"};
std::array<char const*, 2> const LFM_ENGLISH = {"Raid on %target in %minutes min, %count/%max. Whisper me!",
                                                "Who is coming to hit %target? %count/%max, we leave in %minutes min."};

char const* TeamName(uint8 team) { return team == TEAM_ALLIANCE ? "alliance" : "horde"; }

uint8 OtherTeam(uint8 team) { return team == TEAM_ALLIANCE ? uint8(TEAM_HORDE) : uint8(TEAM_ALLIANCE); }

// A spoken line may start with a placeholder (the target's name): the first ASCII letter goes upper case.
std::string Spoken(std::string text)
{
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
        text[0] = char(text[0] - 'a' + 'A');
    return text;
}

bool SameName(std::string const& a, std::string const& b)
{
    auto const same = [](char x, char y) { return std::tolower(uint8(x)) == std::tolower(uint8(y)); };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), same);
}

OutpostRaidState* StateOf(Activity& activity) { return dynamic_cast<OutpostRaidState*>(activity.state.get()); }

// An area or zone in English words (the brain's `where`), whatever the realm's DBC locale.
std::string EnglishAreaName(uint32 id)
{
    AreaTableEntry const* const entry = id ? sAreaTableStore.LookupEntry(id) : nullptr;
    if (!entry)
        return "";
    char const* name = entry->area_name[LOCALE_enUS];
    if (!name || !*name)
        name = entry->area_name[sWorld->GetDefaultDbcLocale()];
    return name ? name : "";
}

ActivityPoint CandidatePoint(ActivityCandidate const& candidate)
{
    ActivityPoint point;
    point.mapId = candidate.mapId;
    point.x = candidate.x;
    point.y = candidate.y;
    point.z = candidate.z;
    point.zoneId = candidate.zoneId;
    point.set = true;
    return point;
}

ActivityPoint PointOfCreature(Creature* creature)
{
    ActivityPoint point;
    point.mapId = creature->GetMapId();
    point.x = creature->GetPositionX();
    point.y = creature->GetPositionY();
    point.z = creature->GetPositionZ();
    point.zoneId = creature->GetZoneId();
    point.set = true;
    return point;
}

// The rally's area in the realm's words (its subzone, else its zone): the Rally and Retreat lines' %area.
std::string RallyArea(ActivityPoint const& rally)
{
    uint32 areaId = 0;
    if (rally.set)
        if (Map* const map = sMapMgr->CreateBaseMap(rally.mapId))
            areaId = map->GetAreaId(PHASEMASK_NORMAL, rally.x, rally.y, rally.z);
    std::string area = ActivityWorld::AreaName(areaId);
    if (area.empty())
        area = ActivityWorld::AreaName(rally.zoneId);
    return area;
}

// The team a request is for: forced, else the player's; none for a console start without one.
std::optional<uint8> RequestTeam(ActivityStartRequest const& request)
{
    if (request.team)
        return *request.team;
    if (request.nearPlayer)
        return uint8(request.nearPlayer->GetTeamId());
    return std::nullopt;
}

void WarnUnknownSpawn(ActivityRaidTarget const& target)
{
    static std::unordered_set<uint32> warned;
    if (warned.insert(target.spawnId).second)
        LOG_WARN(LOG, "CoaActivity.Raid.Targets: {} names spawn {}, which the world does not know: skipped",
                 target.name, target.spawnId);
}

uint64 MinutesMs(uint64 minutes) { return minutes * 60 * 1000; }

using DayCounts = std::map<std::pair<std::string, uint32>, uint32>;

uint32 RaidsToday(DayCounts const& counts, std::string const& name, uint32 day)
{
    auto const it = counts.find({name, day});
    return it == counts.end() ? 0 : it->second;
}

struct RaidFit
{
    ActivityRaidTarget const* target = nullptr;
    ActivitySpawnInfo spawn;
    uint64 lastMs = 0;
};

// Why `target` cannot be raided for `request` now; "" when it can (then `fit` is filled). The census gate and the
// player's level for ambient raids only; the daily cap, the quest-giver guard, the defender-in-zone guard and the
// target's state for every start.
std::string Unfit(ActivityServices& services, ActivityStartRequest const& request, ActivityRaidTarget const& target,
                  std::unordered_map<std::string, uint32> const& inUse, DayCounts const& counts, RaidFit& fit)
{
    ActivityConfig const& config = services.config;
    fit.target = &target;
    if (!ActivityGuards::DescribeSpawn(target.spawnId, fit.spawn))
    {
        WarnUnknownSpawn(target);
        return "spawn unknown";
    }

    if (!request.gm)
    {
        uint32 const need = (config.raidMinBots * ActivityThreshold::RAID_CENSUS_PERCENT + 99) / 100;
        std::optional<uint32> const p10 = services.census.RaidPoolP10(target.team, services.nowUnix);
        if (!p10)
            return Acore::StringFormat("census: fewer than {} samples of this hour",
                                       ActivityThreshold::CENSUS_MIN_HOUR_SAMPLES);
        if (*p10 < need)
            return Acore::StringFormat("census p10 {} < {}", *p10, need);
        Player* const player = request.nearPlayer;
        if (!player)
            return "no player to raid for";
        if (player->GetLevel() + JOIN_LEVEL_MARGIN < config.raidMinLevel)
            return Acore::StringFormat("the player's level {} is below {}", player->GetLevel(),
                                       config.raidMinLevel - JOIN_LEVEL_MARGIN);
    }

    uint32 const today = BrainClock::UtcDay(services.nowUnix);
    uint32 const raids = RaidsToday(counts, target.name, today);
    if (config.raidMaxPerTargetPerDay && raids >= config.raidMaxPerTargetPerDay)
        return Acore::StringFormat("{} raids on it today (Raid.MaxPerTargetPerDay {})", raids,
                                   config.raidMaxPerTargetPerDay);
    if (ActivityGuards::QuestGiverHeld(fit.spawn.entry, services.realPlayers))
        return Acore::StringFormat("a player holds a quest from {}", target.name);
    if (config.raidSkipIfDefenderInZone && ActivityGuards::TeamInZone(services.realPlayers, OtherTeam(target.team),
                                                                      fit.spawn.point.mapId, fit.spawn.point.zoneId))
        return "a defender is in the zone";
    Creature* const creature = ActivityGuards::FindSpawn(target.spawnId);
    if (creature && !creature->IsAlive())
        return "target dead";
    if (inUse.count(target.name))
        return "already raided";
    return "";
}

// The targets that fit `request` now, least recently raided first (config order on ties). `firstWhy`: the first
// target's reason when none fits.
std::vector<RaidFit> Fits(ActivityServices& services, ActivityStartRequest const& request,
                          std::unordered_map<std::string, uint32> const& inUse,
                          std::unordered_map<std::string, uint64> const& lastMs, DayCounts const& counts,
                          std::string& firstWhy)
{
    std::optional<uint8> const team = RequestTeam(request);
    std::vector<RaidFit> fits;
    for (ActivityRaidTarget const& target : services.config.raidTargets)
    {
        if (team && target.team != *team)
            continue;
        if (!request.target.empty() && !SameName(request.target, target.name))
            continue;
        RaidFit fit;
        std::string const why = Unfit(services, request, target, inUse, counts, fit);
        if (!why.empty())
        {
            if (firstWhy.empty())
                firstWhy = target.name + ": " + why;
            continue;
        }
        auto const last = lastMs.find(target.name);
        fit.lastMs = last == lastMs.end() ? 0 : last->second;
        fits.push_back(fit);
    }
    std::stable_sort(fits.begin(), fits.end(), [](RaidFit const& a, RaidFit const& b) { return a.lastMs < b.lastMs; });
    return fits;
}

// The target's rally on the ground: its configured point (probed from above the target), else
// RAID_RALLY_FALLBACK_YARDS away in a random direction.
ActivityPoint RallyOf(ActivityRaidTarget const& target, ActivitySpawnInfo const& spawn)
{
    ActivityPoint const& from = spawn.point;
    if (target.rallySet)
    {
        float const dx = target.rallyX - from.x;
        float const dy = target.rallyY - from.y;
        return ActivityGuards::GroundToward(from, target.rallyX, target.rallyY, std::sqrt(dx * dx + dy * dy));
    }
    ActivityPoint rally;
    for (int attempt = 0; attempt < RALLY_TRIES && !rally.set; ++attempt)
    {
        float const angle = float(rand_norm()) * TWO_PI;
        rally = ActivityGuards::GroundToward(from, from.x + std::cos(angle) * RALLY_DIRECTION_YARDS,
                                             from.y + std::sin(angle) * RALLY_DIRECTION_YARDS,
                                             ActivityThreshold::RAID_RALLY_FALLBACK_YARDS);
    }
    return rally;
}

bool IsIn(Activity const& activity, ObjectGuid guid)
{
    ActivityMember const* const member = activity.FindMember(guid);
    return member && member->In();
}

bool GroupFights(Activity const& activity, Creature* creature)
{
    if (!creature->IsInCombat())
        return false;
    Unit* const victim = creature->GetVictim();
    Unit* const owner = victim ? victim->GetCharmerOrOwnerOrSelf() : nullptr;
    return owner && IsIn(activity, owner->GetGUID());
}

uint32 BotsAlive(Activity const& activity)
{
    uint32 alive = 0;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.isBot || !member.In())
            continue;
        Player* const bot = ActivityWorld::FindMember(member);
        if (bot && bot->IsAlive())
            ++alive;
    }
    return alive;
}

uint32 ArrivedBots(Activity const& activity)
{
    uint32 arrived = 0;
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot && member.arrived)
            ++arrived;
    return arrived;
}

// The first real player member still in the activity's group (the heir of the lead).
Player* RealMemberInGroup(Activity const& activity)
{
    Group* const group =
        activity.groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
    if (!group)
        return nullptr;
    for (ActivityMember const& member : activity.members)
    {
        if (member.isBot || !member.In())
            continue;
        Player* const player = ActivityWorld::FindMember(member);
        if (player && group->IsMember(player->GetGUID()))
            return player;
    }
    return nullptr;
}

void NoteTarget(OutpostRaidState& state, ObjectGuid guid)
{
    if (state.targetGuid == guid)
        return;
    state.targetGuid = guid;
    state.iconSet = false;
}

// The skull comes off the target whenever the raid leaves Active, and at the end (OnEnd, before the director
// disbands the group or the real players keep it), but only while it still marks what the raid marked: a player may
// have moved it. Through the leader while it is in the group, else on the group itself (the leader gone).
void ClearSkull(ActivityServices& services, Activity const& activity, OutpostRaidState& state)
{
    state.iconSet = false;
    ObjectGuid const marked = state.skullOn;
    state.skullOn.Clear();
    Group* const group = marked.IsEmpty() || activity.groupGuid.IsEmpty()
                             ? nullptr
                             : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
    if (!group || group->GetTargetIcon(ActivityThreshold::RAID_ICON_SKULL) != marked)
        return;
    if (group->IsMember(activity.leader))
        services.ops.SetIcon(activity.leader, ActivityThreshold::RAID_ICON_SKULL, ObjectGuid::Empty);
    else
        group->SetTargetIcon(ActivityThreshold::RAID_ICON_SKULL, ObjectGuid::Empty, ObjectGuid::Empty);
    LOG_DEBUG(LOG, "activity #{} raid: skull off {}", activity.id, state.targetName);
}

// Where the defender guard looks: the live target, else its spawn.
ActivityPoint GuardPoint(OutpostRaidState const& state, Creature* creature)
{
    return creature && creature->IsInWorld() ? PointOfCreature(creature) : state.spawn.point;
}

bool DefenderNear(ActivityServices& services, OutpostRaidState const& state, Creature* creature)
{
    uint32 const yards = services.config.raidQuestGiverGuardYards;
    return yards &&
           ActivityGuards::TeamNear(services.realPlayers, state.defenders, GuardPoint(state, creature), float(yards));
}

void LineDue(OutpostRaidState& state, RaidLine line, uint64 nowMs)
{
    state.lineDue = line;
    state.lineDueUntilMs = nowMs + LINE_RETRY_MS;
}

uint64 MinutesLeft(uint64 untilMs, uint64 nowMs)
{
    uint64 const left = untilMs > nowMs ? untilMs - nowMs : 0;
    return std::max<uint64>(1, (left + 59999) / 60000);
}

// The leader says the due line; the budget or a fight delays it for LINE_RETRY_MS at most.
void SayDue(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    if (state.lineDue == RaidLine::None)
        return;
    uint64 const now = services.nowMs;
    if (now >= state.lineDueUntilMs)
    {
        LOG_DEBUG(LOG, "activity #{} raid: line {} given up", activity.id, uint32(state.lineDue));
        state.lineDue = RaidLine::None;
        return;
    }

    std::string text;
    ActivityChatTarget where = ActivityChatTarget::Yell;
    ObjectGuid whisperTo;
    switch (state.lineDue)
    {
        case RaidLine::Rally:
            text = ActivityWorld::Line("coa_act_raid_rally", "Rally at %area. We move out soon.",
                                       {{"%area", RallyArea(state.rally)},
                                        {"%minutes", std::to_string(MinutesLeft(state.gatherUntilMs, now))}});
            where = ActivityChatTarget::Party;
            break;
        case RaidLine::Charge:
            text =
                ActivityWorld::Line("coa_act_raid_charge", "Charge! Take down %target!", {{"%target", activity.name}});
            break;
        case RaidLine::Retreat:
            text = ActivityWorld::Line("coa_act_raid_retreat", "Fall back! Regroup at the rally!",
                                       {{"%area", RallyArea(state.rally)}});
            break;
        case RaidLine::Win:
            text = ActivityWorld::Line("coa_act_raid_win", "%target has fallen! Well fought!",
                                       {{"%target", activity.name}});
            break;
        case RaidLine::Lead:
            text = ActivityWorld::Line("coa_act_raid_lead", "The group is yours now. Thanks for coming!");
            where = ActivityChatTarget::Whisper;
            whisperTo = state.leadTo;
            break;
        default:
            state.lineDue = RaidLine::None;
            return;
    }

    ActivityOpResult const result =
        services.ops.PostToChannel(activity.leader, where, Spoken(std::move(text)), whisperTo);
    if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
        return;
    state.lineDue = RaidLine::None;
}

// The text name of the next recruitment post (contract 6.4, step 1): the brain's pick when it answers in time, else
// the dice. False while the question is out.
bool ChooseLineKey(ActivityServices& services, Activity const& activity, OutpostRaidState& state)
{
    if (!state.lineKey.empty())
        return true;
    ActivityOrganizer& organizer = services.organizer;
    ActivityAskState const ask = organizer.State(activity.id, ActivityAsk::LineKey);
    if (ask == ActivityAskState::Pending)
        return false;
    if (ask == ActivityAskState::Ready)
    {
        std::optional<BrainResult> const result = organizer.Take(activity.id, ActivityAsk::LineKey);
        std::optional<std::size_t> const index =
            result ? ActivityBrain::ReadLine(*result, LFM_KEYS.size()) : std::nullopt;
        state.lineKey = LFM_KEYS[index && *index < LFM_KEYS.size() ? *index : urand(0, 1)];
        return true;
    }
    if (organizer.Usable())
    {
        std::vector<std::string> const keys(LFM_KEYS.begin(), LFM_KEYS.end());
        std::vector<std::string> const english(LFM_ENGLISH.begin(), LFM_ENGLISH.end());
        if (organizer.Ask(activity.id, ActivityAsk::LineKey,
                          ActivityBrain::BuildLineJob(ActivityKindId::Raid, EnglishAreaName(state.spawn.point.zoneId),
                                                      keys, english, 0, 0)))
            return false;
    }
    state.lineKey = LFM_KEYS[urand(0, 1)];
    return true;
}

// The leader's LFM post when one is due: "in %minutes min, %count/%max" in the zone channels it is on or World,
// within Chat.MaxPosts, Chat.MinGapSeconds and Chat.RecruitEverySeconds.
void Post(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    ActivityConfig const& config = services.config;
    uint64 const now = services.nowMs;
    if (now < state.nextPostMs || config.chatChannel == ActivityChatChannel::None || !activity.CanPost(config, now))
        return;
    if (!ChooseLineKey(services, activity, state))
        return;

    uint32 const count = activity.BotsIn() + activity.PlayersIn();
    std::size_t const index = state.lineKey == LFM_KEYS[1] ? 1 : 0;
    std::string const text =
        Spoken(ActivityWorld::Line(LFM_KEYS[index], LFM_ENGLISH[index],
                                   {{"%target", activity.name},
                                    {"%zone", ActivityWorld::AreaName(state.spawn.point.zoneId)},
                                    {"%minutes", std::to_string(MinutesLeft(state.recruitUntilMs, now))},
                                    {"%count", std::to_string(count)},
                                    {"%max", std::to_string(std::max(config.raidMaxBots, count))}}));
    ActivityOpResult const result = config.chatChannel == ActivityChatChannel::Zone
                                        ? services.ops.PostToChannels(activity.leader, config.chatChannels, text)
                                        : services.ops.PostToChannel(activity.leader, ActivityChatTarget::World, text);
    LOG_DEBUG(LOG, "activity #{} raid: LFM {} {}", activity.id, state.lineKey, ActivityOpResultName(result));
    if (result == ActivityOpResult::Deferred)
        return;
    if (result == ActivityOpResult::Done)
    {
        activity.NotePost(now);
        state.nextPostMs = now + uint64(std::max(config.chatMinGapSeconds, config.chatRecruitEverySeconds)) * 1000;
        state.lineKey.clear();
    }
    else
        state.nextPostMs = now + POST_RETRY_MS;
}

void EnterForming(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    state.formingUntilMs = services.nowMs + FORMING_MS;
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Forming, services.nowMs);
}

void EnterGathering(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    uint64 const now = services.nowMs;
    state.gatherUntilMs = now + uint64(services.config.travelGatherTimeoutSeconds) * 1000;
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetFlags(0);
    activity.SetCenter(state.rally, ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetPhase(ActivityPhase::Gathering, now);
    LineDue(state, RaidLine::Rally, now);
}

void EnterMarching(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    Creature* const creature = ActivityGuards::FindSpawn(state.spawn.spawnId);
    ClearSkull(services, activity, state);
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetCenter(creature && creature->IsAlive() ? PointOfCreature(creature) : state.spawn.point, ENGAGE_YARDS);
    activity.SetTravel(ActivityTravel::FollowLeader);
    activity.SetFlags(0);  // the director's regroup sets HOLD while Marching
    activity.SetPhase(ActivityPhase::Marching, services.nowMs);
}

void EnterActive(ActivityServices& services, Activity& activity, OutpostRaidState& state, Creature* creature)
{
    uint64 const now = services.nowMs;
    NoteTarget(state, creature->GetGUID());
    state.engagedSinceMs = now;
    state.nextRetreatAskMs = now + RETREAT_ASK_MS;
    state.engaged = false;
    activity.SetObjective(creature->GetGUID());
    activity.SetCenter(PointOfCreature(creature), ActivityThreshold::OBJECTIVE_REACH_YARDS);
    activity.SetTravel(ActivityTravel::FollowLeader);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Active, now);
    LineDue(state, RaidLine::Charge, now);
}

// Back to the rally (fighting or not), to regroup (a retreat) or to wait for a defender to go (the guard).
void FallBack(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    // SetTravel(Gather) from FollowLeader resets every `arrived`: the director counts arrivals at the rally again.
    // The next engagement marks the target again.
    ClearSkull(services, activity, state);
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetFlags(ACTIVITY_VIEW_RETREAT);
    activity.SetCenter(state.rally, ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
}

// The end of the fight: the win line and the closing emote after a kill; otherwise, when the raid was at the target,
// the bots break off to the rally (RETREAT keeps them from fighting on while the end runs).
void EnterResolving(ActivityServices& services, Activity& activity, OutpostRaidState& state, bool fallBack)
{
    uint64 const now = services.nowMs;
    state.guardRetreat = false;
    ClearSkull(services, activity, state);  // the leader is still in the group: Disbanding comes after
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetTravel(ActivityTravel::None);
    if (fallBack)
    {
        activity.SetCenter(state.rally, ActivityThreshold::RALLY_ARRIVE_RADIUS);
        activity.SetFlags(ACTIVITY_VIEW_RETREAT);
    }
    else
        activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Resolving, now);
    if (!state.won)
        return;

    LineDue(state, RaidLine::Win, now);
    std::vector<uint32> const emotes(SUCCESS_EMOTES.begin(), SUCCESS_EMOTES.end());
    ActivityOrganizer& organizer = services.organizer;
    if (organizer.Usable() && organizer.Ask(activity.id, ActivityAsk::ClosingEmote,
                                            ActivityBrain::BuildEmoteJob(ActivityKindId::Raid, true, emotes, 0, 0)))
        state.emotePending = true;
    else
        activity.SetFlags(ACTIVITY_VIEW_CELEBRATE);
}

void EndWith(ActivityServices& services, Activity& activity, OutpostRaidState& state, ActivityOutcome outcome,
             std::string why, bool fallBack)
{
    LOG_DEBUG(LOG, "activity #{} raid: {} ({})", activity.id, why, ActivityOutcomeName(outcome));
    state.endOutcome = outcome;
    state.endWhy = std::move(why);
    EnterResolving(services, activity, state, fallBack);
}

void ReadClosingEmote(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    if (!state.emotePending)
        return;
    ActivityOrganizer& organizer = services.organizer;
    if (organizer.State(activity.id, ActivityAsk::ClosingEmote) == ActivityAskState::Pending)
        return;
    std::optional<BrainResult> const result = organizer.Take(activity.id, ActivityAsk::ClosingEmote);
    std::optional<std::size_t> const index =
        result ? ActivityBrain::ReadEmote(*result, SUCCESS_EMOTES.size()) : std::nullopt;
    if (index && *index < SUCCESS_EMOTES.size())
        activity.SetClosingEmote(SUCCESS_EMOTES[*index]);
    state.emotePending = false;
    activity.SetFlags(activity.viewFlags | ACTIVITY_VIEW_CELEBRATE);
}

// The defender guard in Marching and Active: a real defender within Raid.QuestGiverGuardYards of the target sends the
// raid back to the rally, where it holds (Gathering) until the defender goes or DEFENDER_HOLD_MS passes.
bool GuardRetreat(ActivityServices& services, Activity& activity, OutpostRaidState& state, Creature* creature)
{
    if (!DefenderNear(services, state, creature))
        return false;
    if (!state.guardHoldSinceMs)
        state.guardHoldSinceMs = services.nowMs;
    state.guardRetreat = true;
    LOG_DEBUG(LOG, "activity #{} raid: a defender is near {}, back to the rally", activity.id, state.targetName);
    FallBack(services, activity, state);
    return true;
}

// Recruiting: the group forms at once (FormGroup converts it to a raid before the 6th member); the leader posts the
// LFM until Raid.RecruitMinutes pass.
void Recruit(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    ActivityConfig const& config = services.config;
    uint64 const now = services.nowMs;
    if (activity.groupGuid.IsEmpty())
    {
        if (now >= state.formSinceMs + ActivityThreshold::FORM_TIMEOUT_MS)
        {
            activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: no group in time", activity.name));
            return;
        }
        std::vector<ObjectGuid> members;
        for (ActivityMember const& member : activity.members)
            if (member.In() && member.isBot && member.guid != activity.leader)
                members.push_back(member.guid);

        // The director's member pass drops a member without the activity's group once the group exists: wait for
        // a tick whose budget adds everyone (when MaxOpsPerTick can).
        uint32 const cost = 1 + uint32(members.size());
        if (cost <= config.maxOpsPerTick && services.ops.Used() + cost > config.maxOpsPerTick)
            return;

        ObjectGuid groupGuid;
        std::vector<ObjectGuid> added;
        std::vector<ObjectGuid> skipped;
        ActivityOpResult const result = services.ops.FormGroup(activity.leader, members, config.raidLootMethod,
                                                               config.raidLootThreshold, groupGuid, added, skipped);
        if (result == ActivityOpResult::Deferred)
            return;
        if (result != ActivityOpResult::Done || groupGuid.IsEmpty())
        {
            activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: group not formed ({})", activity.name,
                                                                         ActivityOpResultName(result)));
            return;
        }
        activity.groupGuid = groupGuid;
        activity.viewDirty = true;
        state.toAdd = std::move(skipped);
        activity.SetFlags(ACTIVITY_VIEW_RECRUITING);
        LOG_DEBUG(LOG, "activity #{} raid: group {} formed, {} added, {} left for later", activity.id,
                  groupGuid.GetCounter(), added.size(), state.toAdd.size());
        return;
    }

    if (now >= state.recruitUntilMs)
    {
        EnterForming(services, activity, state);
        return;
    }
    Post(services, activity, state);
}

// A real player who joined lately is still on the way to the rally (Travel.RallyWaitSeconds after joining).
bool PlayerComing(ActivityServices& services, Activity const& activity)
{
    float const radius = ActivityThreshold::RALLY_ARRIVE_RADIUS;
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || member.isBot ||
            services.nowUnix >= member.joinedUnix + services.config.travelRallyWaitSeconds)
            continue;
        Player* const player = ActivityWorld::FindMember(member);
        if (player && (player->GetMapId() != activity.center.mapId ||
                       activity.center.SqDist2d(player->GetPositionX(), player->GetPositionY()) > radius * radius))
            return true;
    }
    return false;
}

// Gathering (GATHER): at the rally before the march, regrouping there after a retreat, or holding there while a
// defender is near the target.
void Gather(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    ActivityConfig const& config = services.config;
    uint64 const now = services.nowMs;
    // Falling back (a retreat or the defender guard): the end keeps RETREAT until the bots reach the rally.
    bool const fallingBack = (activity.viewFlags & ACTIVITY_VIEW_RETREAT) != 0;

    Creature* const creature = ActivityGuards::FindSpawn(state.spawn.spawnId);
    if (DefenderNear(services, state, creature))
    {
        if (!state.guardHoldSinceMs)
            state.guardHoldSinceMs = now;
        if (now >= state.guardHoldSinceMs + DEFENDER_HOLD_MS)
            EndWith(services, activity, state, ActivityOutcome::Guarded,
                    Acore::StringFormat("a defender stayed near {}", activity.name), fallingBack);
        return;
    }
    if (state.guardHoldSinceMs)
    {
        state.guardHoldSinceMs = 0;
        if (state.guardRetreat)
        {
            state.guardRetreat = false;
            EnterMarching(services, activity, state);
            return;
        }
    }

    if (now >= state.timeoutAtMs)
    {
        EndWith(services, activity, state, ActivityOutcome::Timeout,
                Acore::StringFormat("{} not killed in time", activity.name), fallingBack);
        return;
    }

    bool const together = ArrivedBots(activity) * 100 >= activity.BotsIn() * ActivityThreshold::REGROUP_PERCENT;
    if (activity.viewFlags & ACTIVITY_VIEW_RETREAT)
    {
        if (together || now >= activity.phaseSinceMs + REGROUP_WAIT_MS)
            EnterMarching(services, activity, state);
        return;
    }

    if (together && !PlayerComing(services, activity))
    {
        EnterMarching(services, activity, state);
        return;
    }
    if (now < state.gatherUntilMs)
        return;

    // Time is up: bots still far from the rally stay behind.
    float const stragglerYards = float(config.travelStragglerYards);
    for (ActivityMember const& member : activity.members)
    {
        if (!member.In() || !member.isBot || member.arrived || member.guid == activity.leader)
            continue;
        Player* const bot = ActivityWorld::FindMember(member);
        if (!bot || bot->GetMapId() != state.rally.mapId ||
            state.rally.SqDist2d(bot->GetPositionX(), bot->GetPositionY()) > stragglerYards * stragglerYards)
            activity.MarkLeft(member.guid, ActivityLeaveReason::Dropped, services.nowUnix);
    }
    if (activity.BotsIn() < 2 && activity.PlayersIn() == 0)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: too few came to the rally", activity.name));
        return;
    }
    EnterMarching(services, activity, state);
}

// Marching (MARCH): the leader walks up to the target; Active once close, or as soon as the target fights one of the
// group, and only while no guard holds. A dead or unloaded target: the raid waits at its spawn.
void March(ActivityServices& services, Activity& activity, OutpostRaidState& state, Player* leader)
{
    Creature* const creature = ActivityGuards::FindSpawn(state.spawn.spawnId);
    if (GuardRetreat(services, activity, state, creature))
        return;
    if (services.nowMs >= state.timeoutAtMs)
    {
        EndWith(services, activity, state, ActivityOutcome::Timeout,
                Acore::StringFormat("{} not killed in time", activity.name), false);
        return;
    }
    if (!creature || !creature->IsAlive())
        return;

    NoteTarget(state, creature->GetGUID());
    if (creature->GetMapId() == activity.center.mapId &&
        activity.center.SqDist2d(creature->GetPositionX(), creature->GetPositionY()) >
            CENTER_FOLLOW_YARDS * CENTER_FOLLOW_YARDS)
        activity.SetCenter(PointOfCreature(creature), ENGAGE_YARDS);

    bool const close = leader->GetMapId() == creature->GetMapId() && leader->GetExactDist2d(creature) <= ENGAGE_YARDS;
    if (close || GroupFights(activity, creature))
        EnterActive(services, activity, state, creature);
}

// A retreat: back to the rally to regroup; the second one ends the raid.
void Retreat(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    ++state.retreats;
    LineDue(state, RaidLine::Retreat, services.nowMs);
    if (state.retreats >= 2)
    {
        EndWith(services, activity, state, ActivityOutcome::TooFew,
                Acore::StringFormat("{}: beaten back", activity.name), true);
        return;
    }
    LOG_DEBUG(LOG, "activity #{} raid: retreat {}", activity.id, state.retreats);
    FallBack(services, activity, state);
}

// Active: the retreat question every RETREAT_ASK_MS (Organizer), the dice when the brain is off, late or unsure.
void RetreatCheck(ActivityServices& services, Activity& activity, OutpostRaidState& state, Creature* creature)
{
    uint64 const now = services.nowMs;
    ActivityOrganizer& organizer = services.organizer;
    ActivityAskState const ask = organizer.State(activity.id, ActivityAsk::Retreat);
    if (ask == ActivityAskState::Pending)
        return;

    std::optional<bool> decision;
    if (ask == ActivityAskState::Ready)
    {
        std::optional<BrainResult> const result = organizer.Take(activity.id, ActivityAsk::Retreat);
        // Asked before this engagement (a fall-back came in between): stale, the next check asks again.
        if (state.retreatAskedMs < state.engagedSinceMs)
            return;
        if (result)
            decision = ActivityBrain::ReadRetreat(*result);
    }
    else
    {
        if (now < state.nextRetreatAskMs)
            return;
        if (organizer.Usable())
        {
            ActivityBrain::RetreatFeatures features;
            features.botsIn = activity.BotsIn();
            features.botsAlive = BotsAlive(activity);
            features.playersIn = activity.PlayersIn();
            features.targetHealthPct = uint8(std::clamp<float>(creature->GetHealthPct(), 0.0f, 100.0f));
            features.engagedSeconds = uint32((now - std::min(now, state.engagedSinceMs)) / 1000);
            features.defenderNear = DefenderNear(services, state, creature);
            if (organizer.Ask(activity.id, ActivityAsk::Retreat,
                              ActivityBrain::BuildRetreatJob(ActivityKindId::Raid, features, 0, 0)))
            {
                state.retreatAskedMs = now;
                state.nextRetreatAskMs = now + RETREAT_ASK_MS;
                return;
            }
        }
    }

    state.nextRetreatAskMs = now + RETREAT_ASK_MS;
    bool const fallBack = decision ? *decision : BotsAlive(activity) * 100 < activity.BotsIn() * RETREAT_ALIVE_PERCENT;
    if (fallBack)
        Retreat(services, activity, state);
}

// Active (ENGAGED): the objective until it dies or the time is up, under the guards and the retreat rule.
void Fight(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    Creature* const creature = ActivityGuards::FindSpawn(state.spawn.spawnId);
    if (!creature)
    {
        // Despawned: a kill when it was fighting, else it is looked for again from the march.
        if (state.engaged)
        {
            state.won = true;
            EnterResolving(services, activity, state, false);
        }
        else
            EnterMarching(services, activity, state);
        return;
    }
    if (!creature->IsAlive())
    {
        state.won = true;
        EnterResolving(services, activity, state, false);
        return;
    }
    // A kill of this tick counts; otherwise no fighting on while a defender is near.
    if (GuardRetreat(services, activity, state, creature))
        return;
    if (services.nowMs >= state.timeoutAtMs)
    {
        EndWith(services, activity, state, ActivityOutcome::Timeout,
                Acore::StringFormat("{} not killed in time", activity.name), true);
        return;
    }

    NoteTarget(state, creature->GetGUID());
    activity.SetObjective(creature->GetGUID());
    if (creature->IsInCombat() || creature->GetHealth() < creature->GetMaxHealth())
        state.engaged = true;
    if (creature->GetMapId() == activity.center.mapId &&
        activity.center.SqDist2d(creature->GetPositionX(), creature->GetPositionY()) >
            CENTER_FOLLOW_YARDS * CENTER_FOLLOW_YARDS)
        activity.SetCenter(PointOfCreature(creature), ActivityThreshold::OBJECTIVE_REACH_YARDS);
    if (!state.iconSet)
    {
        ActivityOpResult const result =
            services.ops.SetIcon(activity.leader, ActivityThreshold::RAID_ICON_SKULL, creature->GetGUID());
        LOG_DEBUG(LOG, "activity #{} raid: skull on {} {}", activity.id, state.targetName,
                  ActivityOpResultName(result));
        state.iconSet = true;
        if (result == ActivityOpResult::Done)
            state.skullOn = creature->GetGUID();
    }

    RetreatCheck(services, activity, state, creature);
}

// Disbanding: the lead went to a real player; one bot leaves every DISBAND_STEP_MS, the leader last.
void Disband(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    if (activity.BotsIn() == 0)
    {
        activity.Finish(
            state.won ? ActivityOutcome::Success : state.endOutcome,
            state.won ? Acore::StringFormat("{} killed, the lead handed over", activity.name) : state.endWhy);
        return;
    }
    if (services.nowMs < state.nextLeaveMs)
        return;

    ObjectGuid next;
    for (auto it = activity.members.rbegin(); it != activity.members.rend(); ++it)
    {
        if (it->isBot && it->In() && it->guid != activity.leader)
        {
            next = it->guid;
            break;
        }
    }
    if (next.IsEmpty())
        next = activity.leader;
    activity.MarkLeft(next, ActivityLeaveReason::Ended, services.nowUnix);
    state.nextLeaveMs = services.nowMs + DISBAND_STEP_MS;
}

void EnterDisbanding(ActivityServices& services, Activity& activity, OutpostRaidState& state, Player* heir)
{
    ActivityOpResult const result = services.ops.HandLeader(activity.groupGuid, heir->GetGUID());
    LOG_DEBUG(LOG, "activity #{} raid: lead to player {} {}", activity.id, heir->GetGUID().GetCounter(),
              ActivityOpResultName(result));
    state.leadTo = heir->GetGUID();
    state.nextLeaveMs = services.nowMs;
    LineDue(state, RaidLine::Lead, services.nowMs);
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetTravel(ActivityTravel::None);
    activity.SetPhase(ActivityPhase::Disbanding, services.nowMs);
}

// Bots FormGroup left out for the budget (only when MaxOpsPerTick is below the raid's size).
void AddLate(ActivityServices& services, Activity& activity, OutpostRaidState& state)
{
    if (state.toAdd.empty() || activity.groupGuid.IsEmpty() || activity.phase == ActivityPhase::Resolving ||
        activity.phase == ActivityPhase::Disbanding)
        return;
    for (auto it = state.toAdd.begin(); it != state.toAdd.end();)
    {
        if (!IsIn(activity, *it))
        {
            it = state.toAdd.erase(it);
            continue;
        }
        ActivityOpResult const result = services.ops.AddMember(activity.groupGuid, activity.leader, *it);
        if (result == ActivityOpResult::Deferred)
            break;
        if (result != ActivityOpResult::Done)
            activity.MarkLeft(*it, ActivityLeaveReason::Dropped, services.nowUnix);
        it = state.toAdd.erase(it);
    }
}
}  // namespace

std::optional<std::vector<std::string>> OutpostRaidKind::Targets(ActivityServices& services,
                                                                 ActivityStartRequest const& request)
{
    COA_ASSERT_WORLD_THREAD();
    std::string firstWhy;
    std::vector<RaidFit> const fits = Fits(services, request, targetInUse, lastRaidMs, startsPerDay, firstWhy);
    std::vector<std::string> names;
    for (RaidFit const& fit : fits)
    {
        if (names.size() >= MAX_TARGETS)
            break;
        names.push_back(fit.target->name);
    }
    if (names.empty() && !firstWhy.empty())
        LOG_DEBUG(LOG, "raid targets: none ({})", firstWhy);
    return names;
}

bool OutpostRaidKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                           std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    ActivityConfig const& config = services.config;

    RaidFit fit;
    if (!request.target.empty())
    {
        ActivityRaidTarget const* const target = config.FindRaidTarget(request.target);
        if (!target)
        {
            why = Acore::StringFormat("unknown target {} (Raid.Targets)", request.target);
            return false;
        }
        std::optional<uint8> const team = RequestTeam(request);
        if (team && target->team != *team)
        {
            why = Acore::StringFormat("{} is raided by the {}", target->name, TeamName(target->team));
            return false;
        }
        std::string const unfit = Unfit(services, request, *target, targetInUse, startsPerDay, fit);
        if (!unfit.empty())
        {
            why = target->name + ": " + unfit;
            return false;
        }
    }
    else
    {
        std::string firstWhy;
        std::vector<RaidFit> const fits = Fits(services, request, targetInUse, lastRaidMs, startsPerDay, firstWhy);
        if (fits.empty())
        {
            why = firstWhy.empty() ? std::string("no raid target fits")
                                   : Acore::StringFormat("no raid target fits ({})", firstWhy);
            return false;
        }
        fit = fits.front();
    }
    ActivityRaidTarget const& target = *fit.target;
    uint8 const team = target.team;
    uint8 const minLevel = uint8(std::clamp<uint32>(config.raidMinLevel, 1, TOP_LEVEL));

    uint32 wanted = std::clamp(request.size ? request.size : config.raidMaxBots, config.raidMinBots,
                               std::max(config.raidMinBots, config.raidMaxBots));
    wanted = std::min(wanted, request.capacity);
    uint32 const needed = config.raidMinBots;
    if (wanted < needed)
    {
        why = Acore::StringFormat("only {} participant places left, a raid needs {}", request.capacity, needed);
        return false;
    }

    ActivityPoint const rally = RallyOf(target, fit.spawn);
    if (!rally.set)
    {
        why = Acore::StringFormat("{}: no ground at the rally", target.name);
        return false;
    }

    // The leader near the player, so its LFM reaches the player's zone channel. War Mode raids: no bot whose
    // ruleset War Mode cannot replace, the leader included.
    Player* const nearPlayer = request.nearPlayer;
    ActivityCandidateQuery leaderQuery;
    leaderQuery.team = team;
    leaderQuery.minLevel = minLevel;
    leaderQuery.maxLevel = TOP_LEVEL;
    leaderQuery.center = nearPlayer ? ActivityWorld::PointOf(nearPlayer) : rally;
    leaderQuery.sameMapOnly = nearPlayer != nullptr;
    leaderQuery.maxDistance = nearPlayer ? LEADER_NEAR_YARDS : 0.0f;
    leaderQuery.limit = LEADER_POOL;
    leaderQuery.rulesetSafe = config.pvpWarMode;
    std::vector<ActivityCandidate> pool = services.candidates.Find(leaderQuery);
    if (pool.empty() && request.gm && nearPlayer)
    {
        // A GM start far from any raider: the leader comes from around the rally.
        leaderQuery.center = rally;
        leaderQuery.sameMapOnly = false;
        leaderQuery.maxDistance = 0.0f;
        pool = services.candidates.Find(leaderQuery);
    }
    if (pool.empty())
    {
        why = Acore::StringFormat("no free {} bot of level {}+ near the player", TeamName(team), minLevel);
        return false;
    }
    // The bravest, preferring one in the player's zone (its channel) and out of the player's sight (the director may
    // teleport it to the rally only then).
    uint32 const playerZone = nearPlayer ? nearPlayer->GetZoneId() : 0;
    auto const rank = [playerZone](ActivityCandidate const& candidate)
    {
        bool const sameZone = playerZone && candidate.zoneId == playerZone;
        bool const unseen = candidate.distance < 0.0f || candidate.distance > ActivityThreshold::TELEPORT_UNSEEN_YARDS;
        return std::make_tuple(sameZone, unseen, candidate.bravery);
    };
    std::stable_sort(pool.begin(), pool.end(),
                     [&rank](ActivityCandidate const& a, ActivityCandidate const& b) { return rank(a) > rank(b); });
    ActivityCandidate const leader = pool.front();

    std::unordered_set<uint32> const exclude{leader.guid.GetCounter()};
    ActivityCandidateQuery crewQuery;
    crewQuery.team = team;
    crewQuery.minLevel = minLevel;
    crewQuery.maxLevel = TOP_LEVEL;
    crewQuery.center = rally;
    crewQuery.limit = 2 * wanted - 1;
    crewQuery.rulesetSafe = config.pvpWarMode;
    crewQuery.exclude = &exclude;
    std::vector<ActivityCandidate> const crew = services.candidates.Find(crewQuery);
    if (crew.size() + 1 < needed)
    {
        why = Acore::StringFormat("only {} free {} bots of level {}+", crew.size() + 1, TeamName(team), minLevel);
        return false;
    }

    std::string name = ActivityGuards::CreatureName(fit.spawn.entry);
    if (name.empty())
        name = target.name;

    plan.kind = ActivityKindId::Raid;
    plan.gm = request.gm;
    plan.team = team;
    plan.minLevel = minLevel;
    plan.maxLevel = TOP_LEVEL;
    plan.rally = rally;
    plan.target = target.name;
    plan.name = name;
    plan.wanted = wanted;
    plan.needed = needed;
    plan.leaderFixed = true;
    plan.candidates.clear();
    plan.candidates.push_back(leader);
    plan.candidates.insert(plan.candidates.end(), crew.begin(), crew.end());
    plan.decisions.clear();

    auto state = std::make_unique<OutpostRaidState>();
    state->targetName = target.name;
    state->spawn = fit.spawn;
    state->defenders = OtherTeam(team);
    state->rally = rally;
    LOG_DEBUG(LOG, "raid plan: {} (spawn {}), {} level {}+, leader {}, {} candidates, wanted {}, rally {:.0f} {:.0f}",
              target.name, fit.spawn.spawnId, TeamName(team), minLevel, leader.guid.GetCounter(),
              plan.candidates.size(), wanted, rally.x, rally.y);
    plan.state = std::move(state);
    return true;
}

bool OutpostRaidKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    OutpostRaidState* const state = dynamic_cast<OutpostRaidState*>(plan.state.get());
    if (!state || plan.candidates.empty() || plan.decisions.empty() || plan.decisions[0] != 1)
        return false;
    // Another plan for the same target began first (Raid.Max above 1).
    if (targetInUse.count(state->targetName))
        return false;

    std::vector<ActivityCandidate const*> accepted{&plan.candidates[0]};
    for (std::size_t i = 1; i < plan.candidates.size() && accepted.size() < plan.wanted; ++i)
        if (i < plan.decisions.size() && plan.decisions[i] == 1)
            accepted.push_back(&plan.candidates[i]);
    if (accepted.size() < plan.needed)
        return false;

    ActivityConfig const& config = services.config;
    uint64 const now = services.nowMs;
    activity.leader = accepted.front()->guid;
    activity.name = plan.name;
    activity.target = plan.target;
    activity.minLevel = plan.minLevel;
    activity.maxLevel = plan.maxLevel;
    activity.warMode = config.pvpWarMode;
    for (ActivityCandidate const* candidate : accepted)
        activity.Enroll(candidate->guid, candidate->epoch, true, candidate->level, services.nowUnix);
    activity.hardDeadlineMs = now + MinutesMs(config.raidRecruitMinutes + config.raidTimeoutMinutes +
                                              ActivityThreshold::HARD_DEADLINE_EXTRA_MIN);

    // The daily cap counts starts (GM ones too); older days are forgotten.
    uint32 const today = BrainClock::UtcDay(services.nowUnix);
    for (auto it = startsPerDay.begin(); it != startsPerDay.end();)
    {
        if (it->first.second != today)
            it = startsPerDay.erase(it);
        else
            ++it;
    }
    ++startsPerDay[{state->targetName, today}];
    lastRaidMs[state->targetName] = now;
    targetInUse[state->targetName] = activity.id;

    state->formSinceMs = now;
    state->recruitUntilMs = now + MinutesMs(config.raidRecruitMinutes);
    state->timeoutAtMs = state->recruitUntilMs + MinutesMs(config.raidTimeoutMinutes);
    state->nextPostMs = now;

    ActivityPoint const leaderPoint = CandidatePoint(*accepted.front());
    activity.state = std::move(plan.state);
    activity.SetCenter(leaderPoint, FORMING_RADIUS);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(0);  // RECRUITING once the group exists
    activity.SetPhase(ActivityPhase::Recruiting, now);
    return true;
}

void OutpostRaidKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;

    OutpostRaidState* const state = StateOf(activity);
    if (!state)
    {
        activity.Finish(ActivityOutcome::TooFew, "raid without a target");
        return;
    }

    uint64 const now = services.nowMs;
    // Resolving: RESOLVE_MS whoever is left, then the lead to a real player member (Disbanding) or the end.
    if (activity.phase == ActivityPhase::Resolving)
    {
        ReadClosingEmote(services, activity, *state);
        if (now >= activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS)
        {
            if (Player* const heir = RealMemberInGroup(activity))
                EnterDisbanding(services, activity, *state, heir);
            else
            {
                if (state->won)
                    activity.Finish(ActivityOutcome::Success,
                                    Acore::StringFormat("{} killed, {} retreats", activity.name, state->retreats));
                else
                    activity.Finish(state->endOutcome, state->endWhy);
                return;
            }
        }
        SayDue(services, activity, *state);
        return;
    }
    if (activity.phase == ActivityPhase::Disbanding)
    {
        SayDue(services, activity, *state);
        Disband(services, activity, *state);
        return;
    }

    if (activity.BotsIn() < 2 && activity.PlayersIn() == 0)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: fewer than 2 bots left", activity.name));
        return;
    }

    // The quest-giver guard, before anything of this tick could lead to an attack: a real player holds a quest from
    // the target (to hand in or started there).
    if (ActivityGuards::QuestGiverHeld(state->spawn.entry, services.realPlayers))
    {
        // At the target or falling back from it (Gathering with RETREAT): Resolving keeps RETREAT to the rally.
        bool const atTarget = activity.phase == ActivityPhase::Marching || activity.phase == ActivityPhase::Active ||
                              (activity.viewFlags & ACTIVITY_VIEW_RETREAT) != 0;
        EndWith(services, activity, *state, ActivityOutcome::Guarded,
                Acore::StringFormat("a player holds a quest from {}", activity.name), atTarget);
        SayDue(services, activity, *state);
        return;
    }

    // A leader not found was marked gone by the director's pass just before: OnMemberLeft ended the raid.
    ActivityMember* const leaderMember = activity.Leader();
    Player* const leader = leaderMember && leaderMember->In() ? ActivityWorld::FindMember(*leaderMember) : nullptr;
    if (!leader)
        return;

    switch (activity.phase)
    {
        case ActivityPhase::Recruiting:
            Recruit(services, activity, *state);
            break;
        case ActivityPhase::Forming:
            if (!state->toAdd.empty() && now < state->formingUntilMs)
                break;
            if (activity.BotsIn() + activity.PlayersIn() < services.config.raidMinBots)
            {
                activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: not enough raiders", activity.name));
                return;
            }
            EnterGathering(services, activity, *state);
            break;
        case ActivityPhase::Gathering:
            Gather(services, activity, *state);
            break;
        case ActivityPhase::Marching:
            March(services, activity, *state, leader);
            break;
        case ActivityPhase::Active:
            Fight(services, activity, *state);
            break;
        default:
            break;
    }

    if (activity.finishing)
        return;
    AddLate(services, activity, *state);
    SayDue(services, activity, *state);
}

void OutpostRaidKind::OnMemberLeft(ActivityServices& /*services*/, Activity& activity, ActivityMember const& member)
{
    if (OutpostRaidState* const state = StateOf(activity))
        state->toAdd.erase(std::remove(state->toAdd.begin(), state->toAdd.end(), member.guid), state->toAdd.end());

    // Resolving ends by itself; Disbanding removes the bots on purpose, the leader last.
    if (activity.phase == ActivityPhase::Resolving || activity.phase == ActivityPhase::Disbanding)
        return;
    if (member.guid == activity.leader)
    {
        activity.Finish(ActivityOutcome::LeaderGone, Acore::StringFormat("{}: the leader left ({})", activity.name,
                                                                         ActivityLeaveReasonName(member.leftReason)));
        return;
    }
    if (activity.BotsIn() < 2 && activity.PlayersIn() == 0)
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: fewer than 2 bots left", activity.name));
}

std::string OutpostRaidKind::Join(ActivityServices& services, Activity& activity, Player* player)
{
    COA_ASSERT_WORLD_THREAD();
    std::string const none = ActivityWorld::Line("coa_act_join_none", "No bot activity you can join right now.");
    if (!player || activity.finishing)
        return none;
    switch (activity.phase)
    {
        case ActivityPhase::Recruiting:
        case ActivityPhase::Forming:
        case ActivityPhase::Gathering:
            break;
        default:
            return ActivityWorld::Line("coa_act_join_closed", "That group is not recruiting any more.");
    }
    if (activity.groupGuid.IsEmpty())
        return none;
    ActivityMember* const leaderMember = activity.Leader();
    Player* const leader = leaderMember && leaderMember->In() ? ActivityWorld::FindMember(*leaderMember) : nullptr;
    if (!leader)
        return none;

    ActivityOpResult const result = services.ops.InvitePlayer(activity.leader, player->GetGUID());
    LOG_DEBUG(LOG, "activity #{} raid: invite of {} {}", activity.id, player->GetGUID().GetCounter(),
              ActivityOpResultName(result));
    if (result == ActivityOpResult::Refused)
        return ActivityWorld::Line("coa_act_join_grouped", "Leave your group first.");
    if (result != ActivityOpResult::Done)
        return none;

    activity.invites[player->GetGUID()] = services.nowMs + ActivityThreshold::INVITE_TIMEOUT_MS;
    return ActivityWorld::Line("coa_act_join_group",
                               "%leader invites you to the group for %target: accept the invitation.",
                               {{"%leader", ActivityWorld::FirstName(leader)}, {"%target", activity.name}});
}

bool OutpostRaidKind::Accepts(ActivityServices& /*services*/, Activity const& activity, Player* player)
{
    if (!player || activity.finishing || activity.groupGuid.IsEmpty() || player->GetGroup())
        return false;
    if (activity.phase != ActivityPhase::Recruiting && activity.phase != ActivityPhase::Forming)
        return false;
    return player->GetLevel() + JOIN_LEVEL_MARGIN >= activity.minLevel;
}

void OutpostRaidKind::OnEnd(ActivityServices& services, Activity& activity)
{
    OutpostRaidState* const state = StateOf(activity);
    if (!state)
        return;
    // Before the director disbands the group (or its real players keep it): no skull left behind.
    ClearSkull(services, activity, *state);
    auto const it = targetInUse.find(state->targetName);
    if (it != targetInUse.end() && it->second == activity.id)
        targetInUse.erase(it);
    LOG_DEBUG(LOG, "activity #{} raid: {} ends in {}, won {}, retreats {}", activity.id, state->targetName,
              ActivityPhaseName(activity.phase), state->won, state->retreats);
    state->toAdd.clear();
    state->lineDue = RaidLine::None;
}

std::string OutpostRaidKind::Describe(Activity const& activity, uint64 nowMs) const
{
    OutpostRaidState const* const state = dynamic_cast<OutpostRaidState const*>(activity.state.get());
    if (!state)
        return "raid without a target";
    uint64 const holdMs = state->guardHoldSinceMs ? nowMs - std::min(nowMs, state->guardHoldSinceMs) : 0;
    uint32 const today = RaidsToday(startsPerDay, state->targetName, BrainClock::UtcDay(int64(time(nullptr))));
    uint32 const cap = ActivityConfig::Current()->raidMaxPerTargetPerDay;
    return Acore::StringFormat("raid {} ({}, spawn {}), attackers {}, retreats {}, guard hold {} s, raids today {}/{}",
                               state->targetName, state->spawn.entry, state->spawn.spawnId, TeamName(activity.team),
                               state->retreats, holdMs / 1000, today,
                               cap ? std::to_string(cap) : std::string("no cap"));
}

std::unique_ptr<ActivityKind> CreateOutpostRaidKind() { return std::make_unique<OutpostRaidKind>(); }
