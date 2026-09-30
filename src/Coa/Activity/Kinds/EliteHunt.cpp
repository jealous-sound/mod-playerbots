/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EliteHunt.h"
#include "ActivityBrain.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "ActivityOrganizer.h"
#include "CoaThread.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>

namespace
{
constexpr char const* LOG = "playerbots.activity";
constexpr float HUNT_RALLY_YARDS = 60.0f;        // the rally: this far from the target, on the ground
constexpr float ENGAGE_YARDS = 45.0f;            // Marching ends with the leader this close to the target
constexpr uint64 RETREAT_CHECK_MS = 15000;       // Active: the wipe check
constexpr uint32 RETREAT_ALIVE_PERCENT = 40;     // fewer bots alive than this share of the bots in: fall back
constexpr uint64 REGROUP_WAIT_MS = 90000;        // a fall-back regroups at the rally this long at most
constexpr uint64 LINE_RETRY_MS = 15000;          // a leader's line is retried this long (budget, combat)
constexpr uint32 LEADER_POOL = 40;               // free bots of the band looked at for the leader
constexpr float FORMING_RADIUS = 30.0f;          // Forming and Recruiting: the center is the leader's spot
constexpr float RALLY_DIRECTION_YARDS = 100.0f;  // the rally's direction: toward a random point this far
constexpr int RALLY_TRIES = 4;
constexpr float CENTER_FOLLOW_YARDS = 15.0f;  // Marching/Active: the center follows the target once it moved this far
constexpr uint32 JOIN_LEVEL_MARGIN = 5;
constexpr uint64 POST_RETRY_MS = 60000;  // a refused or failed recruitment post is tried again this much later
constexpr std::size_t MAX_TARGETS = 4;
constexpr float TWO_PI = 6.28318530718f;
// A hunt stays a party (a raid group gets no credit for the targets' non-raid quests): one seat is always left for
// a player.
constexpr uint32 PARTY_SIZE = MAXGROUPSIZE;
constexpr uint32 MAX_HUNT_BOTS = PARTY_SIZE - 1;

std::array<uint32, 6> const SUCCESS_EMOTES = {TEXT_EMOTE_CHEER, TEXT_EMOTE_ROAR,    TEXT_EMOTE_FLEX,
                                              TEXT_EMOTE_DANCE, TEXT_EMOTE_VICTORY, TEXT_EMOTE_APPLAUD};
std::array<char const*, 2> const LFM_KEYS = {"coa_act_lfm_hunt", "coa_act_lfm_hunt_alt"};
std::array<char const*, 2> const LFM_ENGLISH = {"Going after %target, who is in? Whisper me.",
                                                "Group forming to kill %target. Whisper me inv."};

char const* TeamName(uint8 team) { return team == TEAM_ALLIANCE ? "alliance" : "horde"; }

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

EliteHuntState* StateOf(Activity& activity) { return dynamic_cast<EliteHuntState*>(activity.state.get()); }

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

// The team a request is for: forced, else the player's; none for a console start without one.
std::optional<uint8> RequestTeam(ActivityStartRequest const& request)
{
    if (request.team)
        return *request.team;
    if (request.nearPlayer)
        return uint8(request.nearPlayer->GetTeamId());
    return std::nullopt;
}

// One LOG_WARN per unknown spawn for the process (a Hunt.Targets typo, a world without that creature).
void WarnUnknownSpawn(ActivityHuntTarget const& target)
{
    static std::unordered_set<uint32> warned;
    if (warned.insert(target.spawnId).second)
        LOG_WARN(LOG, "CoaActivity.Hunt.Targets: {} names spawn {}, which the world does not know: skipped",
                 target.name, target.spawnId);
}

struct Band
{
    uint8 minLevel = 1;
    uint8 maxLevel = 80;
};

// The bots' levels: the target's (or the GM's forced) level - Hunt.LevelBelow .. + Hunt.LevelAbove, within 1-80.
Band BandOf(ActivityConfig const& config, uint8 level)
{
    int32 const low = std::clamp<int32>(int32(level) - int32(config.huntLevelBelow), 1, 80);
    int32 const high = std::clamp<int32>(int32(level) + int32(config.huntLevelAbove), 1, 80);
    return {uint8(low), uint8(std::max(low, high))};
}

struct HuntFit
{
    ActivityHuntTarget const* target = nullptr;
    ActivitySpawnInfo spawn;
    Band band;
    uint64 lastMs = 0;
};

// Why `target` cannot be hunted for `request` now; "" when it can (then `fit` is filled). The quest guard last: it
// reads every real player's quest log.
std::string Unfit(ActivityServices& services, ActivityStartRequest const& request, ActivityHuntTarget const& target,
                  std::unordered_map<std::string, uint32> const& inUse, HuntFit& fit)
{
    fit.target = &target;
    if (!ActivityGuards::DescribeSpawn(target.spawnId, fit.spawn))
    {
        WarnUnknownSpawn(target);
        return "spawn unknown";
    }
    if (inUse.count(target.name))
        return "already hunted";
    Creature* const creature = ActivityGuards::FindSpawn(target.spawnId);
    if (creature && !creature->IsAlive())
        return "target dead";
    fit.band = BandOf(services.config, request.level ? request.level : fit.spawn.level);
    if (!request.gm)
    {
        Player* const player = request.nearPlayer;
        if (!player || player->GetMapId() != fit.spawn.point.mapId)
            return "no player on its continent";
        uint32 const level = player->GetLevel();
        if (level + JOIN_LEVEL_MARGIN < fit.band.minLevel || level > uint32(fit.band.maxLevel) + JOIN_LEVEL_MARGIN)
            return Acore::StringFormat("the player's level {} is far from {}-{}", level, fit.band.minLevel,
                                       fit.band.maxLevel);
    }
    if (ActivityGuards::QuestTargetNeeded(fit.spawn.entry, services.realPlayers, nullptr))
        return Acore::StringFormat("a player needs {} for a quest", target.name);
    return "";
}

// The targets that fit `request` now, least recently hunted first (config order on ties). `firstWhy`: the first
// target's reason when none fits.
std::vector<HuntFit> Fits(ActivityServices& services, ActivityStartRequest const& request,
                          std::unordered_map<std::string, uint32> const& inUse,
                          std::unordered_map<std::string, uint64> const& lastMs, std::string& firstWhy)
{
    std::optional<uint8> const team = RequestTeam(request);
    std::vector<HuntFit> fits;
    for (ActivityHuntTarget const& target : services.config.huntTargets)
    {
        if (team && target.team != *team)
            continue;
        if (!request.target.empty() && !SameName(request.target, target.name))
            continue;
        HuntFit fit;
        std::string const why = Unfit(services, request, target, inUse, fit);
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
    std::stable_sort(fits.begin(), fits.end(), [](HuntFit const& a, HuntFit const& b) { return a.lastMs < b.lastMs; });
    return fits;
}

bool IsIn(Activity const& activity, ObjectGuid guid)
{
    ActivityMember const* const member = activity.FindMember(guid);
    return member && member->In();
}

// The target fights one of the group (its victim, or its victim's owner, is a member).
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

void NoteTarget(EliteHuntState& state, ObjectGuid guid)
{
    if (state.targetGuid == guid)
        return;
    state.targetGuid = guid;
    state.iconSet = false;
}

Group* HuntGroup(Activity const& activity)
{
    return activity.groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
}

// Room for one more player without InvitePlayer turning the party into a raid (where the real members would get no
// quest credit for the target, and Guarded's "members share the kill" would not hold). Unexpired invitations of
// others hold their seat.
bool HasRoom(ActivityServices const& services, Activity const& activity, ObjectGuid asking = ObjectGuid::Empty)
{
    Group* const group = HuntGroup(activity);
    if (!group || group->isRaidGroup())
        return false;
    uint32 seats = group->GetMembersCount();
    for (auto const& [invited, expiryMs] : activity.invites)
        if (invited != asking && services.nowMs < expiryMs && !group->IsMember(invited))
            ++seats;
    return seats < PARTY_SIZE;
}

// The skull comes off the target whenever the hunt leaves Active, and at the end (OnEnd, before the director
// disbands or hands over the group), but only while it still marks what the hunt marked: a player may have moved it.
// Through the leader while it is in the group, else on the group itself (the leader gone).
void ClearSkull(ActivityServices& services, Activity const& activity, EliteHuntState& state)
{
    state.iconSet = false;
    ObjectGuid const marked = state.skullOn;
    state.skullOn.Clear();
    Group* const group = marked.IsEmpty() ? nullptr : HuntGroup(activity);
    if (!group || group->GetTargetIcon(ActivityThreshold::RAID_ICON_SKULL) != marked)
        return;
    if (group->IsMember(activity.leader))
        services.ops.SetIcon(activity.leader, ActivityThreshold::RAID_ICON_SKULL, ObjectGuid::Empty);
    else
        group->SetTargetIcon(ActivityThreshold::RAID_ICON_SKULL, ObjectGuid::Empty, ObjectGuid::Empty);
    LOG_DEBUG(LOG, "activity #{} hunt: skull off {}", activity.id, state.targetName);
}

void LineDue(EliteHuntState& state, HuntLine line, uint64 nowMs)
{
    state.lineDue = line;
    state.lineDueUntilMs = nowMs + LINE_RETRY_MS;
}

// The leader says the due line; the budget or a fight delays it for LINE_RETRY_MS at most.
void SayDue(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    if (state.lineDue == HuntLine::None)
        return;
    if (services.nowMs >= state.lineDueUntilMs)
    {
        LOG_DEBUG(LOG, "activity #{} hunt: line {} given up", activity.id, uint32(state.lineDue));
        state.lineDue = HuntLine::None;
        return;
    }

    std::map<std::string, std::string> const placeholders{{"%target", activity.name}};
    std::string text;
    if (state.lineDue == HuntLine::Go)
        text = ActivityWorld::Line("coa_act_hunt_go", "There is %target. Everyone on it!", placeholders);
    else
        text = ActivityWorld::Line("coa_act_hunt_win", "%target is down. Nice work!", placeholders);

    ActivityOpResult const result =
        services.ops.PostToChannel(activity.leader, ActivityChatTarget::Say, Spoken(std::move(text)));
    if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
        return;
    state.lineDue = HuntLine::None;
}

// The text name of the next recruitment post (contract 6.4, step 1): the brain's pick when it answers in time, else
// the dice. False while the question is out.
bool ChooseLineKey(ActivityServices& services, Activity const& activity, EliteHuntState& state)
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
                          ActivityBrain::BuildLineJob(ActivityKindId::Hunt, EnglishAreaName(state.spawn.point.zoneId),
                                                      keys, english, 0, 0)))
            return false;
    }
    state.lineKey = LFM_KEYS[urand(0, 1)];
    return true;
}

// The leader's LFM post when one is due: the zone channels (Chat.Channels) of the real player nearest to it, which it
// joins, else those it is on, or World, within Chat.MaxPosts, Chat.MinGapSeconds and Chat.RecruitEverySeconds.
void Post(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    ActivityConfig const& config = services.config;
    uint64 const now = services.nowMs;
    if (now < state.nextPostMs || config.chatChannel == ActivityChatChannel::None || !activity.CanPost(config, now) ||
        !HasRoom(services, activity))
        return;
    if (!ChooseLineKey(services, activity, state))
        return;

    std::size_t const index = state.lineKey == LFM_KEYS[1] ? 1 : 0;
    std::string const text = Spoken(ActivityWorld::Line(
        LFM_KEYS[index], LFM_ENGLISH[index],
        {{"%target", activity.name}, {"%zone", ActivityWorld::AreaName(state.spawn.point.zoneId)}}));
    // The zone channels of the real player nearest to the leader, the hunt's zone when that player is on none.
    ActivityOpResult const result =
        config.chatChannel == ActivityChatChannel::Zone
            ? services.ops.PostToChannels(
                  activity.leader, config.chatChannels, text, state.spawn.point.zoneId,
                  ActivityWorld::NearestAudience(services.realPlayers, activity.team, activity.leader))
            : services.ops.PostToChannel(activity.leader, ActivityChatTarget::World, text);
    LOG_DEBUG(LOG, "activity #{} hunt: LFM {} {}", activity.id, state.lineKey, ActivityOpResultName(result));
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

void EnterRecruiting(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    state.recruitUntilMs = services.nowMs + uint64(services.config.huntRecruitSeconds) * 1000;
    state.nextPostMs = services.nowMs;
    state.lineKey.clear();
    activity.SetFlags(ACTIVITY_VIEW_RECRUITING);
    activity.SetPhase(ActivityPhase::Recruiting, services.nowMs);
}

void EnterGathering(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    state.gatherUntilMs = services.nowMs + uint64(services.config.travelGatherTimeoutSeconds) * 1000;
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetFlags(0);
    activity.SetCenter(state.rally, ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
}

void EnterMarching(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    Creature* const creature = ActivityGuards::FindSpawn(state.spawn.spawnId);
    ClearSkull(services, activity, state);
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetCenter(creature && creature->IsAlive() ? PointOfCreature(creature) : state.spawn.point, ENGAGE_YARDS);
    activity.SetTravel(ActivityTravel::FollowLeader);
    activity.SetFlags(0);  // the director's regroup sets HOLD while Marching
    activity.SetPhase(ActivityPhase::Marching, services.nowMs);
}

void EnterActive(ActivityServices& services, Activity& activity, EliteHuntState& state, Creature* creature)
{
    uint64 const now = services.nowMs;
    NoteTarget(state, creature->GetGUID());
    state.engagedSinceMs = now;
    state.nextRetreatCheckMs = now + RETREAT_CHECK_MS;
    state.engaged = false;
    activity.SetObjective(creature->GetGUID());
    activity.SetCenter(PointOfCreature(creature), ActivityThreshold::OBJECTIVE_REACH_YARDS);
    activity.SetTravel(ActivityTravel::FollowLeader);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Active, now);
    LineDue(state, HuntLine::Go, now);
}

// The end of the hunt: the win line and the closing emote after a kill; after a wipe or a guard at the target the
// bots break off to the rally (RETREAT keeps them from fighting on while the end runs).
void EnterResolving(ActivityServices& services, Activity& activity, EliteHuntState& state, bool fallBack)
{
    uint64 const now = services.nowMs;
    ClearSkull(services, activity, state);
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

    LineDue(state, HuntLine::Win, now);
    std::vector<uint32> const emotes(SUCCESS_EMOTES.begin(), SUCCESS_EMOTES.end());
    ActivityOrganizer& organizer = services.organizer;
    if (organizer.Usable() && organizer.Ask(activity.id, ActivityAsk::ClosingEmote,
                                            ActivityBrain::BuildEmoteJob(ActivityKindId::Hunt, true, emotes, 0, 0)))
        state.emotePending = true;
    else
        activity.SetFlags(ACTIVITY_VIEW_CELEBRATE);
}

// Resolving: the brain's closing emote once answered (or late), then CELEBRATE.
void ReadClosingEmote(ActivityServices& services, Activity& activity, EliteHuntState& state)
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

// The quest-target guard (every tick until the kill): a real player outside the group who still needs the target
// for a quest ends the hunt before it dies. Players in the group share the kill (the hunt stays a party); so will the
// ones invited (until the invitation expires). At the target, or falling back from it, the bots break off first:
// RETREAT pulls them to the rally for RESOLVE_MS, then Resolving finishes Guarded. Before that nothing fights it.
bool Guarded(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    std::unordered_set<uint32> exempt;
    for (ActivityMember const& member : activity.members)
        if (!member.isBot && member.In())
            exempt.insert(member.guid.GetCounter());
    for (auto const& [invited, expiryMs] : activity.invites)
        if (services.nowMs < expiryMs)
            exempt.insert(invited.GetCounter());
    Player* who = nullptr;
    if (!ActivityGuards::QuestTargetNeeded(state.spawn.entry, services.realPlayers, &exempt, &who))
        return false;
    LOG_DEBUG(LOG, "activity #{} hunt: {} needs {} for a quest", activity.id, who ? who->GetName() : "?",
              state.targetName);
    std::string why = Acore::StringFormat("a player needs {} for a quest", activity.name);
    bool const atTarget = activity.phase == ActivityPhase::Marching || activity.phase == ActivityPhase::Active ||
                          (activity.viewFlags & ACTIVITY_VIEW_RETREAT) != 0;
    if (atTarget)
    {
        state.endOutcome = ActivityOutcome::Guarded;
        state.endWhy = std::move(why);
        EnterResolving(services, activity, state, true);
        return true;
    }
    activity.SetObjective(ObjectGuid::Empty);
    activity.Finish(ActivityOutcome::Guarded, std::move(why));
    return true;
}

// Forming: the leader's group with every member at once when the budget allows it.
void Form(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    if (services.nowMs >= state.formSinceMs + ActivityThreshold::FORM_TIMEOUT_MS)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: no group in time", activity.name));
        return;
    }

    std::vector<ObjectGuid> members;
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot && member.guid != activity.leader)
            members.push_back(member.guid);

    // The director's member pass drops a member without the activity's group once the group exists: wait for a
    // tick whose budget adds everyone (when MaxOpsPerTick can).
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
    LOG_DEBUG(LOG, "activity #{} hunt: group {} formed, {} added, {} left for later", activity.id,
              groupGuid.GetCounter(), added.size(), state.toAdd.size());
    if (services.config.huntRecruitSeconds)
        EnterRecruiting(services, activity, state);
    else
        EnterGathering(services, activity, state);
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

// Gathering: at the rally before the march, or regrouping there after a fall-back (RETREAT).
void Gather(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    ActivityConfig const& config = services.config;
    uint64 const now = services.nowMs;
    uint32 arrived = 0;
    for (ActivityMember const& member : activity.members)
        if (member.In() && member.isBot && member.arrived)
            ++arrived;
    bool const together = arrived * 100 >= activity.BotsIn() * ActivityThreshold::REGROUP_PERCENT;

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

// Marching: the leader walks up to the target; Active once close, or as soon as the target fights one of the group.
// A dead or unloaded target: the group waits at its spawn.
void March(ActivityServices& services, Activity& activity, EliteHuntState& state, Player* leader)
{
    Creature* const creature = ActivityGuards::FindSpawn(state.spawn.spawnId);
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

// Active: the objective until it dies; a wipe check every RETREAT_CHECK_MS (dice: fewer than 40 % of the bots
// alive), the second wipe ends the hunt.
void Fight(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    uint64 const now = services.nowMs;
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
        LOG_DEBUG(LOG, "activity #{} hunt: skull on {} {}", activity.id, state.targetName,
                  ActivityOpResultName(result));
        state.iconSet = true;
        if (result == ActivityOpResult::Done)
            state.skullOn = creature->GetGUID();
    }

    if (now < state.nextRetreatCheckMs)
        return;
    state.nextRetreatCheckMs = now + RETREAT_CHECK_MS;
    uint32 const botsIn = activity.BotsIn();
    uint32 const alive = BotsAlive(activity);
    if (alive * 100 >= botsIn * RETREAT_ALIVE_PERCENT)
        return;

    ++state.retreats;
    LOG_DEBUG(LOG, "activity #{} hunt: {} of {} bots alive, fall-back {}", activity.id, alive, botsIn, state.retreats);
    if (state.retreats >= 2)
    {
        state.endOutcome = ActivityOutcome::TooFew;
        state.endWhy = Acore::StringFormat("{}: wiped", activity.name);
        EnterResolving(services, activity, state, true);
        return;
    }
    // SetTravel(Gather) from FollowLeader resets every `arrived`: the director counts arrivals at the rally again.
    // The skull comes off, or the grinding leader would walk back to it from the rally; the next engagement marks
    // the target again.
    ClearSkull(services, activity, state);
    activity.SetObjective(ObjectGuid::Empty);
    activity.SetFlags(ACTIVITY_VIEW_RETREAT);
    activity.SetCenter(state.rally, ActivityThreshold::RALLY_ARRIVE_RADIUS);
    activity.SetTravel(ActivityTravel::Gather);
    activity.SetPhase(ActivityPhase::Gathering, now);
}

// Bots FormGroup left out for the budget (only when MaxOpsPerTick is below the hunt's size).
void AddLate(ActivityServices& services, Activity& activity, EliteHuntState& state)
{
    if (state.toAdd.empty() || activity.groupGuid.IsEmpty() || activity.phase == ActivityPhase::Resolving)
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

std::optional<std::vector<std::string>> EliteHuntKind::Targets(ActivityServices& services,
                                                               ActivityStartRequest const& request)
{
    COA_ASSERT_WORLD_THREAD();
    std::string firstWhy;
    std::vector<HuntFit> const fits = Fits(services, request, targetInUse, lastHuntMs, firstWhy);
    std::vector<std::string> names;
    for (HuntFit const& fit : fits)
    {
        if (names.size() >= MAX_TARGETS)
            break;
        names.push_back(fit.target->name);
    }
    if (names.empty() && !firstWhy.empty())
        LOG_DEBUG(LOG, "hunt targets: none ({})", firstWhy);
    return names;
}

bool EliteHuntKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                         std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    ActivityConfig const& config = services.config;

    HuntFit fit;
    if (!request.target.empty())
    {
        ActivityHuntTarget const* const target = config.FindHuntTarget(request.target);
        if (!target)
        {
            why = Acore::StringFormat("unknown target {} (Hunt.Targets)", request.target);
            return false;
        }
        std::optional<uint8> const team = RequestTeam(request);
        if (team && target->team != *team)
        {
            why = Acore::StringFormat("{} is hunted by the {}", target->name, TeamName(target->team));
            return false;
        }
        std::string const unfit = Unfit(services, request, *target, targetInUse, fit);
        if (!unfit.empty())
        {
            why = target->name + ": " + unfit;
            return false;
        }
    }
    else
    {
        std::string firstWhy;
        std::vector<HuntFit> const fits = Fits(services, request, targetInUse, lastHuntMs, firstWhy);
        if (fits.empty())
        {
            why = firstWhy.empty() ? std::string("no hunt target fits")
                                   : Acore::StringFormat("no hunt target fits ({})", firstWhy);
            return false;
        }
        fit = fits.front();
    }
    ActivityHuntTarget const& target = *fit.target;
    uint8 const team = target.team;

    uint32 wanted = std::clamp(request.size ? request.size : config.huntMaxBots, config.huntMinBots,
                               std::max(config.huntMinBots, config.huntMaxBots));
    wanted = std::min({wanted, request.capacity, MAX_HUNT_BOTS});
    uint32 const needed = config.huntMinBots;
    if (needed > MAX_HUNT_BOTS)
    {
        why = Acore::StringFormat("Hunt.MinBots {} leaves no seat for a player (a hunt stays a party of {})", needed,
                                  PARTY_SIZE);
        return false;
    }
    if (wanted < needed)
    {
        why = Acore::StringFormat("only {} participant places left, a hunt needs {}", request.capacity, needed);
        return false;
    }

    ActivityCandidateQuery leaderQuery;
    leaderQuery.team = team;
    leaderQuery.minLevel = fit.band.minLevel;
    leaderQuery.maxLevel = fit.band.maxLevel;
    leaderQuery.center = fit.spawn.point;
    leaderQuery.limit = LEADER_POOL;
    std::vector<ActivityCandidate> const pool = services.candidates.Find(leaderQuery);
    if (pool.empty())
    {
        why =
            Acore::StringFormat("no free {} bot of levels {}-{}", TeamName(team), fit.band.minLevel, fit.band.maxLevel);
        return false;
    }
    // The bravest, from the target's zone when one is there: its zone channel is where the players who would come
    // read the LFM.
    std::size_t best = 0;
    for (std::size_t i = 1; i < pool.size(); ++i)
    {
        bool const inZone = pool[i].zoneId == fit.spawn.point.zoneId;
        bool const bestInZone = pool[best].zoneId == fit.spawn.point.zoneId;
        if ((inZone && !bestInZone) || (inZone == bestInZone && pool[i].bravery > pool[best].bravery))
            best = i;
    }
    ActivityCandidate const leader = pool[best];

    std::unordered_set<uint32> const exclude{leader.guid.GetCounter()};
    ActivityCandidateQuery crewQuery = leaderQuery;
    crewQuery.limit = 2 * wanted - 1;
    crewQuery.exclude = &exclude;
    std::vector<ActivityCandidate> const crew = services.candidates.Find(crewQuery);
    if (crew.size() + 1 < needed)
    {
        why = Acore::StringFormat("only {} free bots of levels {}-{}", crew.size() + 1, fit.band.minLevel,
                                  fit.band.maxLevel);
        return false;
    }

    // The rally: HUNT_RALLY_YARDS from the target in a random direction, on the ground.
    ActivityPoint rally;
    for (int attempt = 0; attempt < RALLY_TRIES && !rally.set; ++attempt)
    {
        float const angle = float(rand_norm()) * TWO_PI;
        rally =
            ActivityGuards::GroundToward(fit.spawn.point, fit.spawn.point.x + std::cos(angle) * RALLY_DIRECTION_YARDS,
                                         fit.spawn.point.y + std::sin(angle) * RALLY_DIRECTION_YARDS, HUNT_RALLY_YARDS);
    }
    if (!rally.set)
        rally = ActivityWorld::GroundNear(fit.spawn.point, HUNT_RALLY_YARDS);
    if (!rally.set)
    {
        why = Acore::StringFormat("{}: no ground near the target", target.name);
        return false;
    }

    std::string name = ActivityGuards::CreatureName(fit.spawn.entry);
    if (name.empty())
        name = target.name;

    plan.kind = ActivityKindId::Hunt;
    plan.gm = request.gm;
    plan.team = team;
    plan.minLevel = fit.band.minLevel;
    plan.maxLevel = fit.band.maxLevel;
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

    auto state = std::make_unique<EliteHuntState>();
    state->targetName = target.name;
    state->spawn = fit.spawn;
    state->rally = rally;
    LOG_DEBUG(LOG, "hunt plan: {} (spawn {}, level {}), {} levels {}-{}, leader {}, {} candidates, wanted {}",
              target.name, fit.spawn.spawnId, fit.spawn.level, TeamName(team), fit.band.minLevel, fit.band.maxLevel,
              leader.guid.GetCounter(), plan.candidates.size(), wanted);
    plan.state = std::move(state);
    return true;
}

bool EliteHuntKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    EliteHuntState* const state = dynamic_cast<EliteHuntState*>(plan.state.get());
    if (!state || plan.candidates.empty() || plan.decisions.empty() || plan.decisions[0] != 1)
        return false;
    // Another plan for the same target began first (Hunt.Max above 1).
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
    for (ActivityCandidate const* candidate : accepted)
        activity.Enroll(candidate->guid, candidate->epoch, true, candidate->level, services.nowUnix);
    activity.hardDeadlineMs = now + uint64(config.huntRecruitSeconds) * 1000 +
                              uint64(config.huntMinutes + ActivityThreshold::HARD_DEADLINE_EXTRA_MIN) * 60 * 1000;

    lastHuntMs[state->targetName] = now;
    targetInUse[state->targetName] = activity.id;
    state->formSinceMs = now;

    ActivityPoint const leaderPoint = CandidatePoint(*accepted.front());
    activity.state = std::move(plan.state);
    activity.SetCenter(leaderPoint, FORMING_RADIUS);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(0);
    activity.SetPhase(ActivityPhase::Forming, now);
    return true;
}

void EliteHuntKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    if (activity.finishing)
        return;

    EliteHuntState* const state = StateOf(activity);
    if (!state)
    {
        activity.Finish(ActivityOutcome::TooFew, "hunt without a target");
        return;
    }

    uint64 const now = services.nowMs;
    // Resolving ends within RESOLVE_MS whoever is left: OnMemberLeft ends nothing then, not even for the leader.
    if (activity.phase == ActivityPhase::Resolving)
    {
        ReadClosingEmote(services, activity, *state);
        if (now >= activity.phaseSinceMs + ActivityThreshold::RESOLVE_MS)
        {
            if (state->won)
                activity.Finish(ActivityOutcome::Success,
                                Acore::StringFormat("{} killed, {} fall-backs", activity.name, state->retreats));
            else
                activity.Finish(state->endOutcome, state->endWhy);
            return;
        }
        SayDue(services, activity, *state);
        return;
    }

    if (activity.BotsIn() < 2 && activity.PlayersIn() == 0)
    {
        activity.Finish(ActivityOutcome::TooFew, Acore::StringFormat("{}: fewer than 2 bots left", activity.name));
        return;
    }

    // The quest-target guard, before anything of this tick could lead to a kill.
    if (Guarded(services, activity, *state))
        return;

    // A leader not found was marked gone by the director's pass just before: OnMemberLeft ended the hunt.
    ActivityMember* const leaderMember = activity.Leader();
    Player* const leader = leaderMember && leaderMember->In() ? ActivityWorld::FindMember(*leaderMember) : nullptr;
    if (!leader)
        return;

    switch (activity.phase)
    {
        case ActivityPhase::Forming:
            Form(services, activity, *state);
            break;
        case ActivityPhase::Recruiting:
            if (now >= state->recruitUntilMs)
                EnterGathering(services, activity, *state);
            else
                Post(services, activity, *state);
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

void EliteHuntKind::OnMemberLeft(ActivityServices& /*services*/, Activity& activity, ActivityMember const& member)
{
    if (EliteHuntState* const state = StateOf(activity))
        state->toAdd.erase(std::remove(state->toAdd.begin(), state->toAdd.end(), member.guid), state->toAdd.end());

    // Resolving ends by itself within RESOLVE_MS.
    if (activity.phase == ActivityPhase::Resolving)
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

std::string EliteHuntKind::Join(ActivityServices& services, Activity& activity, Player* player)
{
    COA_ASSERT_WORLD_THREAD();
    std::string const none = ActivityWorld::Line("coa_act_join_none", "No bot activity you can join right now.");
    if (!player || activity.finishing || activity.phase == ActivityPhase::Resolving || activity.groupGuid.IsEmpty())
        return none;
    ActivityMember* const leaderMember = activity.Leader();
    Player* const leader = leaderMember && leaderMember->In() ? ActivityWorld::FindMember(*leaderMember) : nullptr;
    if (!leader)
        return none;
    // A full party: InvitePlayer would make it a raid.
    if (!HasRoom(services, activity, player->GetGUID()))
        return ActivityWorld::Line("coa_act_join_closed", "That group is not recruiting any more.");

    ActivityOpResult const result = services.ops.InvitePlayer(activity.leader, player->GetGUID());
    LOG_DEBUG(LOG, "activity #{} hunt: invite of {} {}", activity.id, player->GetGUID().GetCounter(),
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

bool EliteHuntKind::Accepts(ActivityServices& services, Activity const& activity, Player* player)
{
    if (!player || activity.finishing || activity.groupGuid.IsEmpty() || player->GetGroup() ||
        !HasRoom(services, activity, player->GetGUID()))
        return false;
    switch (activity.phase)
    {
        case ActivityPhase::Forming:
        case ActivityPhase::Recruiting:
        case ActivityPhase::Gathering:
        case ActivityPhase::Marching:
        case ActivityPhase::Active:
            break;
        default:
            return false;
    }
    uint32 const level = player->GetLevel();
    return level + JOIN_LEVEL_MARGIN >= activity.minLevel && level <= uint32(activity.maxLevel) + JOIN_LEVEL_MARGIN;
}

void EliteHuntKind::OnEnd(ActivityServices& services, Activity& activity)
{
    EliteHuntState* const state = StateOf(activity);
    if (!state)
        return;
    // Before the director disbands the group or hands it to its real players: no skull left behind.
    ClearSkull(services, activity, *state);
    auto const it = targetInUse.find(state->targetName);
    if (it != targetInUse.end() && it->second == activity.id)
        targetInUse.erase(it);
    LOG_DEBUG(LOG, "activity #{} hunt: {} ends in {}, won {}, fall-backs {}", activity.id, state->targetName,
              ActivityPhaseName(activity.phase), state->won, state->retreats);
    state->toAdd.clear();
    state->lineDue = HuntLine::None;
}

std::string EliteHuntKind::Describe(Activity const& activity, uint64 /*nowMs*/) const
{
    EliteHuntState const* const state = dynamic_cast<EliteHuntState const*>(activity.state.get());
    if (!state)
        return "hunt without a target";
    Creature* const creature = ActivityGuards::FindSpawn(state->spawn.spawnId);
    char const* const status = !creature ? "not loaded" : creature->IsAlive() ? "alive" : "dead";
    return Acore::StringFormat("hunt {} ({}, spawn {}) level {}, {}, retreats {}, rally {:.0f} {:.0f}",
                               state->targetName, state->spawn.entry, state->spawn.spawnId, state->spawn.level, status,
                               state->retreats, state->rally.x, state->rally.y);
}

std::unique_ptr<ActivityKind> CreateEliteHuntKind() { return std::make_unique<EliteHuntKind>(); }
