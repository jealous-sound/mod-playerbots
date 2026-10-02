/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FestivalVisit.h"
#include "ActivityConfig.h"
#include "ActivityGuards.h"
#include "ActivityPvePolicy.h"
#include "CoaActivityValues.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "Escort.h"
#include "GameEventMgr.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GossipDef.h"
#include "Item.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ScriptedGossip.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
constexpr uint32 SAYGE = 14822;
constexpr uint32 BONFIRE = 16592;
constexpr uint32 RIBBON = 181605;
constexpr uint32 TASTYFISH = 180248;
constexpr uint32 FISHING = 7620;
constexpr uint32 BLOSSOM = 23247;
constexpr uint32 FUEL = 28806;
constexpr uint32 RIBBON_CHANNEL = 45406;
constexpr uint64 ACTION_GAP_MS = 1000;
constexpr uint32 MAX_TASKS = 64;
constexpr float ARRIVE_YARDS = 2.0f;
constexpr float FISHING_YARDS = 12.0f;
// Same faction bonfires from core scripts/Events/midsummer.cpp; never the city's PvP desecration path.
constexpr std::array<uint32, 40> ALLIANCE_FIRES =
    {187564, 187914, 187916, 187917, 187919, 187920, 187921, 187922, 187923, 187924,
     187925, 187926, 187927, 187928, 187929, 187930, 187931, 187932, 187933, 187934,
     187935, 187936, 187937, 187938, 187939, 187940, 187941, 187942, 187943, 187944,
     187945, 187946, 194032, 194035, 194036, 194038, 194040, 194044, 194045, 194049};
constexpr std::array<uint32, 38> HORDE_FIRES =
    {187559, 187947, 187948, 187949, 187950, 187951, 187952, 187953, 187954, 187955,
     187956, 187957, 187958, 187959, 187960, 187961, 187962, 187963, 187964, 187965,
     187966, 187967, 187968, 187969, 187970, 187971, 187972, 187973, 187974, 187975,
     194033, 194034, 194037, 194039, 194042, 194043, 194046, 194048};
constexpr std::array<uint32, 8> FORTUNES = {23768, 23769, 23767, 23738, 23766, 23737, 23735, 23736};

enum class Result : uint8 { Pending, Done, Failed };
struct Task
{
    uint32 activity = 0;
    uint64 epoch = 0;
    uint32 spawn = 0;
    uint32 event = 0;
    uint32 entry = 0;
    std::string interaction;
    ActivityPoint point;
    uint64 untilMs = 0;
    uint64 nextMs = 0;
    uint32 stage = 0;
    uint32 oldBlossoms = 0;
    bool issued = false;
    bool cancelled = false;
    Result result = Result::Pending;
};

// Only plain tasks/results cross threads. Native operations always run after copying and releasing the mutex;
// spell hooks may synchronously read the task again while a native handler executes.
std::mutex taskMutex;
std::unordered_map<uint32, Task> tasks;
std::atomic<bool> anyTasks{false};

struct FestivalState : ActivityKindState
{
    Task task;
    ActivityPoint camp;
    ObjectGuid bot;
    uint64 epoch = 0;
};

FestivalState* StateOf(Activity& activity) { return dynamic_cast<FestivalState*>(activity.state.get()); }

bool EventActive(uint32 event)
{
    return event && event <= 65535 && event < sGameEventMgr->GetEventMap().size() &&
           sGameEventMgr->IsActiveEvent(uint16(event));
}

bool GetTask(uint32 low, Task& out)
{
    if (!anyTasks.load(std::memory_order_relaxed))
        return false;
    std::lock_guard<std::mutex> lock(taskMutex);
    auto const found = tasks.find(low);
    if (found == tasks.end())
        return false;
    out = found->second;
    return true;
}

void PutTask(uint32 low, Task const& task)
{
    std::lock_guard<std::mutex> lock(taskMutex);
    auto const found = tasks.find(low);
    if (found != tasks.end() && found->second.activity == task.activity && found->second.epoch == task.epoch &&
        !found->second.cancelled)
        found->second = task;
}

bool Install(uint32 low, Task const& task, uint64 now)
{
    std::lock_guard<std::mutex> lock(taskMutex);
    for (auto it = tasks.begin(); it != tasks.end();)
        if (it->second.untilMs + 60000 < now)
            it = tasks.erase(it);
        else
            ++it;
    if (tasks.size() >= MAX_TASKS && !tasks.count(low))
        return false;
    tasks[low] = task;
    anyTasks.store(true, std::memory_order_relaxed);
    return true;
}

void Cancel(uint32 low, uint32 id)
{
    std::lock_guard<std::mutex> lock(taskMutex);
    auto const found = tasks.find(low);
    if (found != tasks.end() && found->second.activity == id)
        found->second.cancelled = true;
}

bool FishingReady(Player* bot)
{
    Item* const pole = bot ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND) : nullptr;
    return bot && bot->HasSpell(FISHING) && bot->GetSkillValue(SKILL_FISHING) && pole &&
           pole->GetTemplate()->Class == ITEM_CLASS_WEAPON &&
           pole->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
}

bool Materials(Player* bot, Task const& task)
{
    if (task.interaction == "fishing")
        return FishingReady(bot);
    if (task.interaction == "bonfire")
        return bot->HasItemCount(BLOSSOM, 1, false);
    if (task.interaction == "faire")
        for (uint32 spell : FORTUNES)
            if (bot->HasSpellCooldown(spell))
                return false;
    return true;
}

bool DescribeTarget(ActivityFestivalTarget const& target, Task& task)
{
    task = Task();
    task.spawn = target.spawnId;
    task.event = target.eventId;
    task.interaction = target.interaction;
    if (!EventActive(task.event))
        return false;
    if (task.interaction == "faire" || task.interaction == "bonfire")
    {
        ActivitySpawnInfo spawn;
        if (!ActivityGuards::DescribeSpawn(task.spawn, spawn))
            return false;
        task.entry = spawn.entry;
        task.point = spawn.point;
        CreatureTemplate const* const info = sObjectMgr->GetCreatureTemplate(task.entry);
        if (!info || sObjectMgr->GetScriptName(info->ScriptID) !=
                         (task.interaction == "faire" ? "npc_sayge" : "npc_midsummer_bonfire"))
            return false;
    }
    else
    {
        GameObjectData const* const data = sObjectMgr->GetGameObjectData(task.spawn);
        if (!data)
            return false;
        task.entry = data->id;
        task.point.mapId = data->mapid;
        task.point.x = data->posX;
        task.point.y = data->posY;
        task.point.z = data->posZ;
        task.point.set = true;
        GameObjectTemplate const* const info = sObjectMgr->GetGameObjectTemplate(task.entry);
        if (!info || (task.interaction == "ribbon" &&
                      (info->type != GAMEOBJECT_TYPE_GOOBER || info->goober.spellId != RIBBON_CHANNEL)) ||
            (task.interaction == "fishing" && info->type != GAMEOBJECT_TYPE_FISHINGHOLE))
            return false;
    }
    MapEntry const* const map = sMapStore.LookupEntry(task.point.mapId);
    return map && !map->Instanceable() && Acore::IsValidMapCoord(task.point.x, task.point.y, task.point.z) &&
           FestivalVisitActions::Supported(task.interaction, task.event, task.entry);
}

ActivityPoint Camp(Task const& task, ActivityCandidate const& candidate)
{
    if (task.interaction == "fishing")
        return ActivityGuards::GroundToward(task.point, candidate.x, candidate.y, FISHING_YARDS);
    return ActivityGuards::GroundToward(task.point, candidate.x, candidate.y, 2.0f);
}

std::vector<ActivityCandidate> Candidates(ActivityServices& services, Task const& task, uint8 team)
{
    ActivityCandidateQuery query;
    query.team = team;
    query.minLevel = 1;
    query.maxLevel = 60;
    query.center = task.point;
    query.sameMapOnly = true;
    query.maxDistance = float(services.config.festivalNearbyYards);
    query.rulesetSafe = true;
    query.limit = 16;
    auto found = services.candidates.Find(query);
    found.erase(std::remove_if(found.begin(), found.end(), [&](ActivityCandidate const& candidate)
    {
        Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
        return !ActivityPvePolicy::SafePlayer(bot) || !Materials(bot, task) || !Camp(task, candidate).set;
    }), found.end());
    if (found.size() > 2)
        found.resize(2);
    return found;
}

Creature* Npc(Player* bot, Task const& task)
{
    auto const range = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(task.spawn);
    for (auto it = range.first; it != range.second; ++it)
    {
        Creature* const npc = it->second;
        if (npc && npc->IsInWorld() && npc->IsAlive() && npc->GetEntry() == task.entry && bot->InSamePhase(npc) &&
            bot->GetExactDist(npc) <= INTERACTION_DISTANCE && !npc->IsHostileTo(bot) && bot->IsWithinLOSInMap(npc))
            return npc;
    }
    return nullptr;
}

GameObject* Go(Player* bot, Task const& task, float yards)
{
    auto const range = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(task.spawn);
    for (auto it = range.first; it != range.second; ++it)
    {
        GameObject* const go = it->second;
        if (go && go->IsInWorld() && go->isSpawned() && go->GetEntry() == task.entry && bot->InSamePhase(go) &&
            bot->GetExactDist(go) <= yards && bot->IsWithinLOSInMap(go))
            return go;
    }
    return nullptr;
}

bool OwnFire(Player* bot, Creature* npc)
{
    if (bot->GetTeamId() == TEAM_ALLIANCE)
        for (uint32 entry : ALLIANCE_FIRES)
            if (npc->FindNearestGameObject(entry, 10.0f))
                return true;
    if (bot->GetTeamId() == TEAM_HORDE)
        for (uint32 entry : HORDE_FIRES)
            if (npc->FindNearestGameObject(entry, 10.0f))
                return true;
    return false;
}

void StopOwnChannel(Player* bot, Task const& task)
{
    if (!task.issued)
        return;
    Spell* const channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    uint32 const spell = task.interaction == "fishing" ? FISHING : task.interaction == "ribbon" ? RIBBON_CHANNEL : 0;
    if (channel && spell && channel->GetSpellInfo()->Id == spell)
        bot->InterruptSpell(CURRENT_CHANNELED_SPELL, false, true, true);
    // Ribbon is a native periodic aura rather than a channel on some data revisions; remove only our visual aura.
    if (task.interaction == "ribbon")
        bot->RemoveAurasDueToSpell(RIBBON_CHANNEL);
}

bool SelectSayge(Player* bot, Creature* npc, uint32 sender, uint32 action)
{
    if (!bot->PlayerTalkClass)
        return false;
    GossipMenu& menu = bot->PlayerTalkClass->GetGossipMenu();
    if (menu.GetSenderGUID() != npc->GetGUID())
        return false;
    for (auto const& option : menu.GetMenuItems())
    {
        if (option.second.Sender != sender || option.second.OptionType != action ||
            option.second.IsCoded || option.second.BoxMoney)
            continue;
        WorldPacket packet(CMSG_GOSSIP_SELECT_OPTION);
        packet << npc->GetGUID() << menu.GetMenuId() << option.first << std::string();
        bot->GetSession()->HandleGossipSelectOptionOpcode(packet);
        return true;
    }
    return false;
}

void Interact(PlayerbotAI* ai, Task& task, uint64 now)
{
    Player* const bot = ai->GetBot();
    if (task.interaction == "faire")
    {
        Creature* const npc = Npc(bot, task);
        if (!npc)
            return;
        if (!task.stage)
        {
            WorldPacket packet(CMSG_GOSSIP_HELLO);
            packet << npc->GetGUID();
            bot->GetSession()->HandleGossipHelloOpcode(packet);
            task.issued = true;
        }
        else if (task.stage <= 3)
        {
            // Local npc_sayge: hello -> let the thief go -> share credit/gold -> Spirit buff.
            uint32 const sender = task.stage == 3 ? GOSSIP_SENDER_MAIN + 4 : GOSSIP_SENDER_MAIN;
            uint32 const action = task.stage == 1 ? GOSSIP_ACTION_INFO_DEF + 1 :
                                  task.stage == 2 ? GOSSIP_ACTION_INFO_DEF + 5 : GOSSIP_ACTION_INFO_DEF;
            if (!SelectSayge(bot, npc, sender, action))
            {
                task.result = Result::Failed;
                return;
            }
        }
        else
        {
            // Stop before the fortune-item choice: native buff/cooldown is the verified interaction outcome.
            task.result = bot->HasAura(23738) && bot->HasSpellCooldown(23738) ? Result::Done : Result::Failed;
            return;
        }
        ++task.stage;
    }
    else if (task.interaction == "bonfire")
    {
        Creature* const npc = Npc(bot, task);
        if (!npc || !OwnFire(bot, npc))
            return;
        if (!task.issued)
        {
            Item* const blossom = bot->GetItemByEntry(BLOSSOM);
            if (!blossom || blossom->GetTemplate()->Spells[0].SpellId != int32(FUEL))
            {
                task.result = Result::Failed;
                return;
            }
            task.oldBlossoms = bot->GetItemCount(BLOSSOM, false);
            WorldPacket packet(CMSG_USE_ITEM);
            packet << blossom->GetBagSlot() << blossom->GetSlot() << uint8(1) << FUEL << blossom->GetGUID()
                   << uint32(0) << uint8(0) << uint32(TARGET_FLAG_UNIT) << npc->GetGUID().WriteAsPacked();
            task.issued = true;
            PutTask(bot->GetGUID().GetCounter(), task);  // CanPrepare validates this exact issued task synchronously.
            bot->GetSession()->HandleUseItemOpcode(packet);
            task.nextMs = now + 3000;
        }
        else
            task.result = bot->GetItemCount(BLOSSOM, false) < task.oldBlossoms ? Result::Done : Result::Failed;
    }
    else if (task.interaction == "ribbon")
    {
        GameObject* const pole = Go(bot, task, INTERACTION_DISTANCE);
        if (!pole)
            return;
        if (!task.issued)
        {
            task.issued = true;
            pole->Use(bot);
            task.nextMs = now + 5000;
        }
        else
        {
            bool const applied = bot->HasAura(RIBBON_CHANNEL);
            StopOwnChannel(bot, task);
            task.result = applied ? Result::Done : Result::Failed;
        }
    }
    else if (task.interaction == "fishing")
    {
        GameObject* const pool = Go(bot, task, 20.0f);
        if (!pool || !FishingReady(bot))
            return;
        if (!task.issued)
        {
            bot->SetFacingToObject(pool);
            task.issued = true;
            PutTask(bot->GetGUID().GetCounter(), task);
            if (!ai->CastSpell(FISHING, bot))
            {
                task.result = Result::Failed;
                return;
            }
            task.nextMs = now + 2000;
        }
        else
        {
            Spell* const channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            bool const cast = channel && channel->GetSpellInfo()->Id == FISHING;
            // No go->Use, loot opening, fishing-pole grants, quest acceptance or turn-in, hence no tournament win.
            StopOwnChannel(bot, task);
            task.result = cast ? Result::Done : Result::Failed;
        }
    }
}
}

bool FestivalVisitActions::Supported(std::string const& interaction, uint32 event, uint32 entry)
{
    return (interaction == "faire" && (event == 4 || event == 5) && entry == SAYGE) ||
           (interaction == "ribbon" && event == 1 && entry == RIBBON) ||
           (interaction == "bonfire" && event == 1 && entry == BONFIRE) ||
           (interaction == "fishing" && event == 15 && entry == TASTYFISH);
}

bool FestivalVisitActions::SelfTest()
{
    return Supported("faire", 4, SAYGE) && Supported("faire", 5, SAYGE) && Supported("ribbon", 1, RIBBON) &&
           Supported("bonfire", 1, BONFIRE) && Supported("fishing", 15, TASTYFISH) &&
           !Supported("faire", 23, SAYGE) && !Supported("faire", 3, SAYGE) && !Supported("ribbon", 1, TASTYFISH) &&
           !Supported("fishing", 90, TASTYFISH) && !Supported("bonfire", 1, 1) && !Supported("quest", 1, BONFIRE);
}

bool FestivalVisitActions::AllowsCast(PlayerbotAI* ai, SpellInfo const* spell, Unit* target)
{
    if (!ai || !spell || !target || !ActivityPvePolicy::SafePlayer(ai->GetBot()))
        return false;
    Player* const bot = ai->GetBot();
    ActivityMemberView const* const row = CoaActivityBot::Refresh(ai).row;
    Task task;
    if (!row || row->kind != ActivityKindId::FestivalVisit || row->phase != ActivityPhase::Active ||
        !GetTask(bot->GetGUID().GetCounter(), task) || task.activity != row->activityId ||
        task.epoch != ai->GetCoaEpoch() || task.cancelled || !task.issued || task.untilMs <= CoaTime::NowMs() ||
        !EventActive(task.event) || bot->IsInCombat())
        return false;
    if (task.interaction == "fishing")
        return spell->Id == FISHING && target == bot && FishingReady(bot) && Go(bot, task, 20.0f);
    if (task.interaction == "bonfire")
        return spell->Id == FUEL && target == Npc(bot, task) && OwnFire(bot, target->ToCreature());
    return false;
}

void FestivalVisitActions::Release(PlayerbotAI* ai)
{
    if (!ai)
        return;
    Task task;
    uint32 const low = ai->GetBot()->GetGUID().GetCounter();
    if (!GetTask(low, task) || task.epoch != ai->GetCoaEpoch())
        return;
    StopOwnChannel(ai->GetBot(), task);
    std::lock_guard<std::mutex> lock(taskMutex);
    auto const found = tasks.find(low);
    if (found != tasks.end() && found->second.activity == task.activity && found->second.epoch == task.epoch)
        tasks.erase(found);
    anyTasks.store(!tasks.empty(), std::memory_order_relaxed);
}

void FestivalVisitActions::Tick(PlayerbotAI* ai)
{
    if (!ai)
        return;
    Player* const bot = ai->GetBot();
    Task task;
    if (!GetTask(bot->GetGUID().GetCounter(), task))
        return;
    ActivityMemberView const* const row = CoaActivityBot::Refresh(ai).row;
    if (task.epoch != ai->GetCoaEpoch())
        return;  // A new life never cancels a spell or inherits the old life's task.
    if (task.cancelled || !row || row->activityId != task.activity || row->kind != ActivityKindId::FestivalVisit)
    {
        Release(ai);
        return;
    }
    if (row->phase != ActivityPhase::Active || task.result != Result::Pending)
        return;
    uint64 const now = CoaTime::NowMs();
    if (!ActivityPvePolicy::SafePlayer(bot) || bot->IsInCombat() || !EventActive(task.event) || now >= task.untilMs)
    {
        StopOwnChannel(bot, task);
        task.result = Result::Failed;
        PutTask(bot->GetGUID().GetCounter(), task);
        return;
    }
    if (now < task.nextMs)
        return;
    task.nextMs = now + ACTION_GAP_MS;
    if (!bot->isMoving())
        Interact(ai, task, now);
    PutTask(bot->GetGUID().GetCounter(), task);
}

std::optional<std::vector<std::string>> FestivalVisitKind::Targets(ActivityServices& services,
                                                                 ActivityStartRequest const& request)
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<std::string> names;
    if (!services.config.festivalEnable)
        return names;
    for (ActivityFestivalTarget const& target : services.config.festivalTargets)
    {
        if (request.team && *request.team != target.team)
            continue;
        if (request.nearPlayer && request.nearPlayer->GetTeamId() != target.team)
            continue;
        Task task;
        if (DescribeTarget(target, task) && !Candidates(services, task, target.team).empty())
            names.push_back(target.name);
        if (names.size() >= 4)
            break;
    }
    return names;
}

bool FestivalVisitKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                             std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    if (!services.config.festivalEnable || !services.config.festivalMax || !request.capacity)
    {
        why = "festival visits disabled or without capacity";
        return false;
    }
    std::string name = request.target;
    if (name.empty())
    {
        auto const names = Targets(services, request);
        if (names && !names->empty())
            name = names->front();
    }
    ActivityFestivalTarget const* const target = services.config.FindFestivalTarget(name);
    auto state = std::make_unique<FestivalState>();
    if (!target || (request.team && *request.team != target->team) ||
        (request.nearPlayer && request.nearPlayer->GetTeamId() != target->team) ||
        !DescribeTarget(*target, state->task))
    {
        why = "festival target/event/native interaction is unavailable or unverified";
        return false;
    }
    auto candidates = Candidates(services, state->task, target->team);
    if (candidates.empty())
    {
        why = "no nearby safe qualified visitor with the required learned skill/equipped pole/owned fuel";
        return false;
    }
    state->camp = Camp(state->task, candidates.front());
    if (!state->camp.set)
    {
        why = "festival camp has no dry ground";
        return false;
    }
    plan.kind = Id();
    plan.gm = request.gm;
    plan.team = target->team;
    plan.minLevel = 1;
    plan.maxLevel = 60;
    plan.name = target->name;
    plan.target = target->name;
    plan.rally = state->camp;
    plan.needed = plan.wanted = 1;
    plan.leaderFixed = false;
    plan.candidates = std::move(candidates);
    plan.decisions.clear();
    plan.state = std::move(state);
    return true;
}

bool FestivalVisitKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    FestivalState* const state = dynamic_cast<FestivalState*>(plan.state.get());
    ActivityFestivalTarget const* const target = services.config.FindFestivalTarget(plan.target);
    Task current;
    if (!services.config.festivalEnable || !services.config.festivalMax || !state || !target ||
        !DescribeTarget(*target, current) || current.spawn != state->task.spawn || current.entry != state->task.entry ||
        current.interaction != state->task.interaction || current.event != state->task.event ||
        target->team != plan.team)
        return false;
    for (std::size_t i = 0; i < plan.candidates.size(); ++i)
    {
        if (i >= plan.decisions.size() || plan.decisions[i] != 1)
            continue;
        ActivityCandidate const& candidate = plan.candidates[i];
        Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!ai || ai->GetCoaEpoch() != candidate.epoch || !ActivityCandidates::IsFree(bot) || bot->IsInCombat() ||
            !ActivityPvePolicy::SafePlayer(bot) || bot->GetTeamId() != plan.team ||
            bot->GetMapId() != state->task.point.mapId || !Materials(bot, state->task))
            continue;
        state->camp = Camp(state->task, candidate);
        if (!state->camp.set)
            continue;
        state->bot = candidate.guid;
        state->epoch = candidate.epoch;
        state->task.activity = activity.id;
        state->task.epoch = candidate.epoch;
        state->task.untilMs = services.nowMs + uint64(services.config.festivalMinutes) * 60000;
        if (!Install(candidate.guid.GetCounter(), state->task, services.nowMs))
            return false;
        activity.Enroll(candidate.guid, candidate.epoch, true, candidate.level, services.nowUnix);
        activity.name = plan.name;
        activity.target = plan.target;
        activity.minLevel = 1;
        activity.maxLevel = 60;
        activity.hardDeadlineMs = state->task.untilMs;
        activity.SetCenter(state->camp, ARRIVE_YARDS);
        activity.SetTravel(ActivityTravel::None);
        activity.SetFlags(0);
        activity.SetPhase(ActivityPhase::Gathering, services.nowMs);
        activity.state = std::move(plan.state);
        return true;
    }
    return false;
}

void FestivalVisitKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    FestivalState* const state = StateOf(activity);
    ActivityMember const* const member = state ? activity.FindMember(state->bot) : nullptr;
    Player* const bot = member && member->In() ? ActivityWorld::FindMember(*member) : nullptr;
    if (!services.config.festivalEnable)
    {
        activity.Finish(ActivityOutcome::Disabled, "festival visits disabled");
        return;
    }
    if (!state || !member || member->epoch != state->epoch || !ActivityPvePolicy::SafePlayer(bot) ||
        bot->IsInCombat() || bot->GetMapId() != state->camp.mapId || !EventActive(state->task.event))
    {
        activity.Finish(ActivityOutcome::Guarded, "festival visitor/event became unavailable");
        return;
    }
    if (services.nowMs >= state->task.untilMs)
    {
        activity.Finish(ActivityOutcome::Timeout, "festival interaction deadline reached");
        return;
    }
    if (activity.phase == ActivityPhase::Gathering)
    {
        if (!EscortRules::Arrived(state->camp, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                                   bot->GetPositionZ(), ARRIVE_YARDS))
            return;
        activity.SetFlags(ACTIVITY_VIEW_QUIET);
        activity.SetPhase(ActivityPhase::Active, services.nowMs);
    }
    Task task;
    if (!GetTask(state->bot.GetCounter(), task) || task.activity != activity.id || task.epoch != state->epoch)
        activity.Finish(ActivityOutcome::Guarded, "festival interaction task lost");
    else if (task.result == Result::Done)
        activity.Finish(ActivityOutcome::Success, "verified native " + task.interaction + " interaction completed");
    else if (task.result == Result::Failed)
        activity.Finish(ActivityOutcome::Guarded, "native festival interaction unavailable or refused");
}

void FestivalVisitKind::OnMemberLeft(ActivityServices&, Activity& activity, ActivityMember const&)
{
    activity.Finish(ActivityOutcome::TooFew, "festival visitor left");
}

std::string FestivalVisitKind::Join(ActivityServices&, Activity&, Player*)
{
    return ActivityWorld::Line("coa_act_festival_solo", "This visitor participates alone; the event remains yours.");
}

void FestivalVisitKind::OnEnd(ActivityServices&, Activity& activity)
{
    FestivalState* const state = StateOf(activity);
    if (state)
        Cancel(state->bot.GetCounter(), activity.id);
}

std::string FestivalVisitKind::Describe(Activity const& activity, uint64 nowMs) const
{
    FestivalState const* const state = dynamic_cast<FestivalState const*>(activity.state.get());
    if (!state)
        return "festival state missing";
    return Acore::StringFormat("event {}, {} at spawn {}, {}, {} s left; no quest turn-in or fishing loot",
                               state->task.event, state->task.interaction, state->task.spawn,
                               activity.phase == ActivityPhase::Gathering ? "walking" : "native interaction",
                               state->task.untilMs > nowMs ? (state->task.untilMs - nowMs) / 1000 : 0);
}

std::unique_ptr<ActivityKind> CreateFestivalVisitKind() { return std::make_unique<FestivalVisitKind>(); }
