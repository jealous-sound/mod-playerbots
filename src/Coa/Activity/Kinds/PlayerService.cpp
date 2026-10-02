/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerService.h"
#include "ActivityConfig.h"
#include "ActivityOperations.h"
#include "ActivityPvePolicy.h"
#include "AiObjectContext.h"
#include "CoaActivityValues.h"
#include "CoaThread.h"
#include "CraftValue.h"
#include "DBCStores.h"
#include "Escort.h"
#include "Group.h"
#include "GroupMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include <algorithm>
#include <charconv>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace
{
constexpr uint32 CANDIDATE_POOL = 32;
constexpr uint32 MAX_RECIPES = 2000;
constexpr uint64 LINE_RETRY_MS = 15000;

struct ServiceLease
{
    uint32 activity = 0;
    uint64 epoch = 0;
    ObjectGuid requester;
    uint32 spell = 0;
    uint64 untilMs = 0;
};

std::mutex leaseMutex;
std::unordered_map<uint32, ServiceLease> leases;

bool Lease(uint32 low, ServiceLease const& lease, uint64 nowMs)
{
    std::lock_guard<std::mutex> lock(leaseMutex);
    for (auto it = leases.begin(); it != leases.end();)
        if (it->second.untilMs + 60000 < nowMs)
            it = leases.erase(it);
        else
            ++it;
    if (leases.size() >= 64 && !leases.count(low))
        return false;
    leases[low] = lease;
    return true;
}

struct ServiceState : ActivityKindState
{
    uint32 spell = 0;
    uint32 item = 0;
    uint32 skill = 0;
    uint32 map = 0;
    uint64 formUntilMs = 0;
    uint64 inviteUntilMs = 0;
    uint64 workUntilMs = 0;
    bool invited = false;
    bool accepted = false;
    bool leadership = false;
    std::string line;
    uint64 lineUntilMs = 0;
};

ServiceState* StateOf(Activity& activity) { return dynamic_cast<ServiceState*>(activity.state.get()); }

Player* Requester(Activity const& activity) { return ObjectAccessor::FindConnectedPlayer(activity.requester); }

Group* ServiceGroup(Activity const& activity)
{
    return activity.groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(activity.groupGuid.GetCounter());
}

bool SafeRequester(Player* player, uint32 map, ObjectGuid allowedGroup = ObjectGuid::Empty)
{
    return ActivityPvePolicy::SafePlayer(player) && player->GetMapId() == map && !player->IsInCombat() &&
           player->GetSession() && !player->GetSession()->IsBot() && !player->GetSession()->isLogingOut() &&
           !player->isAFK() && !player->isDND() && !player->GetVehicle() &&
           (!player->GetGroup() || (!allowedGroup.IsEmpty() && player->GetGroup()->GetGUID() == allowedGroup));
}

bool Nearby(Player* bot, Player* requester, ActivityConfig const& config)
{
    return bot && requester && ActivityPvePolicy::SafePlayer(bot) && !bot->IsInCombat() && !bot->GetVehicle() &&
           bot->GetMap() == requester->GetMap() && bot->InSamePhase(requester) &&
           bot->GetTeamId() == requester->GetTeamId() && bot->GetExactDist(requester) <= config.serviceNearbyYards;
}

ActivityCandidateQuery Query(Player* requester, ActivityConfig const& config)
{
    ActivityCandidateQuery query;
    query.team = uint8(requester->GetTeamId());
    query.minLevel = 1;
    query.maxLevel = 60;
    query.center = ActivityWorld::PointOf(requester);
    query.sameMapOnly = true;
    query.maxDistance = float(config.serviceNearbyYards);
    query.rulesetSafe = true;
    query.limit = CANDIDATE_POOL;
    return query;
}

std::optional<PlayerServiceCatalog::Recipe> Learned(Player* bot, uint32 spell)
{
    auto recipes = PlayerServiceCatalog::Recipes(bot, MAX_RECIPES);
    auto const found = std::find_if(recipes.begin(), recipes.end(), [spell](auto const& recipe)
    {
        return recipe.spellId == spell;
    });
    return found == recipes.end() ? std::nullopt : std::optional<PlayerServiceCatalog::Recipe>(*found);
}

void QueueLine(ActivityServices& services, ServiceState& state, char const* key, char const* fallback,
               std::map<std::string, std::string> values = {})
{
    state.line = ActivityWorld::Line(key, fallback, values);
    state.lineUntilMs = services.nowMs + LINE_RETRY_MS;
}

void PostLine(ActivityServices& services, Activity& activity, ServiceState& state)
{
    if (state.line.empty())
        return;
    if (services.nowMs >= state.lineUntilMs)
    {
        state.line.clear();
        return;
    }
    ActivityOpResult const result = services.ops.PostToChannel(activity.leader, ActivityChatTarget::Whisper,
                                                              state.line, activity.requester);
    if (result != ActivityOpResult::Deferred && result != ActivityOpResult::Refused)
        state.line.clear();
}
}

bool PlayerServiceCatalog::ParseTarget(std::string const& target, uint32& spell, uint32& botLow)
{
    spell = 0;
    botLow = 0;
    auto number = [](char const* first, char const* last, uint32& out)
    {
        if (first == last || *first < '0' || *first > '9')
            return false;
        auto const result = std::from_chars(first, last, out);
        return result.ec == std::errc() && result.ptr == last && out != 0;
    };
    std::size_t const colon = target.find(':');
    char const* const start = target.data();
    char const* const end = start + target.size();
    if (colon == std::string::npos)
        return number(start, end, spell);
    return number(start, start + colon, spell) && number(start + colon + 1, end, botLow);
}

bool PlayerServiceCatalog::SelfTest()
{
    uint32 spell = 0;
    uint32 bot = 0;
    if (!ParseTarget("123:456", spell, bot) || spell != 123 || bot != 456 ||
        !ParseTarget("123", spell, bot) || spell != 123 || bot != 0)
        return false;
    for (char const* invalid : {"", "0", "1:0", "0:1", ":1", "1:", "1:2:3", "-1", "+1", "1 ",
                               " 1", "4294967296", "1:4294967296", "foo"})
        if (ParseTarget(invalid, spell, bot))
            return false;
    return true;
}

void PlayerServiceCatalog::Release(PlayerbotAI* ai)
{
    if (!ai || !ai->GetBot())
        return;
    Player* const bot = ai->GetBot();
    ServiceLease lease;
    {
        std::lock_guard<std::mutex> lock(leaseMutex);
        auto const found = leases.find(bot->GetGUID().GetCounter());
        if (found == leases.end() || found->second.epoch != ai->GetCoaEpoch())
            return;
        lease = found->second;
        leases.erase(found);  // At most one cleanup, including repeated shutdown/destructor release.
    }
    ActivityMemberView const* const row = CoaActivityBot::Refresh(ai).row;
    if (!ai->HasM7ControlledGuard() ||
        (row && (row->kind != ActivityKindId::PlayerService || row->activityId != lease.activity)))
        return;
    CraftData& craft = ai->GetAiObjectContext()->GetValue<CraftData&>("craft")->Get();
    for (CurrentSpellTypes const type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL})
    {
        Spell* const cast = bot->GetCurrentSpell(type);
        if (!cast)
            continue;
        SpellInfo const* const info = cast->GetSpellInfo();
        bool relevant = info->Id == lease.spell;
        for (SpellEffectInfo const& effect : info->Effects)
            relevant = relevant || (craft.itemId && effect.Effect == SPELL_EFFECT_CREATE_ITEM &&
                                     effect.ItemType == craft.itemId);
        if (relevant)
            bot->InterruptSpell(type, false, true, true);
    }
    if (bot->GetSession() && bot->GetTrader() && bot->GetTrader()->GetGUID() == lease.requester)
    {
        WorldPacket packet(CMSG_CANCEL_TRADE);
        bot->GetSession()->HandleCancelTradeOpcode(packet);
    }
    craft.itemId = 0;
    craft.required.clear();
    craft.obtained.clear();
}

std::vector<PlayerServiceCatalog::Recipe> PlayerServiceCatalog::Recipes(Player* bot, uint32 limit)
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<Recipe> result;
    if (!bot || !limit)
        return result;
    limit = std::min(limit, MAX_RECIPES);
    // Iterate actual learned spells, never a trainer/DB wish list. SpellMap is ordered by spell id.
    for (auto const& learned : bot->GetSpellMap())
    {
        if (!learned.second || learned.second->State == PLAYERSPELL_REMOVED || !learned.second->Active ||
            !(learned.second->specMask & bot->GetActiveSpecMask()))
            continue;
        SpellInfo const* const spell = sSpellMgr->GetSpellInfo(learned.first);
        if (!spell || spell->IsPassive())
            continue;
        uint32 item = 0;
        for (auto const& effect : spell->Effects)
            if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && sObjectMgr->GetItemTemplate(effect.ItemType))
            {
                item = effect.ItemType;
                break;
            }
        if (!item)
            continue;
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spell->Id);
        for (auto ability = bounds.first; ability != bounds.second; ++ability)
        {
            SkillLineEntry const* const skill = sSkillLineStore.LookupEntry(ability->second->SkillLine);
            uint32 const value = skill ? bot->GetSkillValue(skill->id) : 0;
            if (!skill || !value || value < ability->second->MinSkillLineRank ||
                (skill->categoryId != SKILL_CATEGORY_PROFESSION && skill->categoryId != SKILL_CATEGORY_SECONDARY))
                continue;
            Recipe recipe;
            recipe.spellId = spell->Id;
            recipe.itemId = item;
            recipe.skillId = skill->id;
            recipe.skillValue = value;
            recipe.profession = skill->name[0] ? skill->name[0] : "profession";
            recipe.recipe = spell->SpellName[0] ? spell->SpellName[0] : "recipe";
            recipe.item = sObjectMgr->GetItemTemplate(item)->Name1;
            recipe.hasReagents = true;
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (spell->Reagent[i] > 0 &&
                    !bot->HasItemCount(uint32(spell->Reagent[i]), spell->ReagentCount[i], false))
                    recipe.hasReagents = false;
            result.push_back(std::move(recipe));
            break;
        }
        if (result.size() >= limit)
            break;
    }
    return result;
}

std::vector<PlayerServiceCatalog::Offer> PlayerServiceCatalog::Offers(ActivityServices& services, Player* requester,
                                                                    uint32 recipeSpell, uint32 limit)
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<Offer> result;
    if (!services.config.serviceEnable || !requester || !limit || !SafeRequester(requester, requester->GetMapId()))
        return result;
    limit = std::min(limit, uint32(100));
    for (ActivityCandidate const& candidate : services.candidates.Find(Query(requester, services.config)))
    {
        Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
        PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!ai || ai->GetCoaEpoch() != candidate.epoch || !Nearby(bot, requester, services.config))
            continue;
        for (Recipe const& recipe : Recipes(bot, MAX_RECIPES))
        {
            if (recipeSpell && recipe.spellId != recipeSpell)
                continue;
            result.push_back({candidate.guid, candidate.epoch, bot->GetName(), recipe});
            if (result.size() >= limit)
                return result;
        }
    }
    return result;
}

std::string PlayerServiceCatalog::FormatOffer(Offer const& offer)
{
    std::string const stock = ActivityWorld::Line(offer.learned.hasReagents ? "coa_act_service_stock" :
                                                                          "coa_act_service_no_stock",
        offer.learned.hasReagents ? "Reagents present; tools and cooldown not checked." :
                                   "Reagents needed; tools and cooldown not checked.");
    return ActivityWorld::Line("coa_act_service_offer",
        "%bot [%botid], %profession %skill: %item (item %itemid, recipe %recipe:%botid; %stock). "
        "Arrange crafting after accepting the party invitation.",
        {{"%bot", offer.botName}, {"%botid", std::to_string(offer.bot.GetCounter())},
         {"%profession", offer.learned.profession}, {"%skill", std::to_string(offer.learned.skillValue)},
         {"%item", offer.learned.item}, {"%itemid", std::to_string(offer.learned.itemId)},
         {"%recipe", std::to_string(offer.learned.spellId)}, {"%stock", stock}});
}

std::vector<std::string> PlayerServiceCatalog::Lines(ActivityServices& services, Player* requester,
                                                    uint32 recipeSpell, uint32 limit)
{
    std::vector<std::string> lines;
    for (Offer const& offer : Offers(services, requester, recipeSpell, limit))
        lines.push_back(FormatOffer(offer));
    if (lines.empty())
        lines.push_back(ActivityWorld::Line("coa_act_service_none", "No qualified nearby free crafter is available."));
    return lines;
}

bool PlayerServiceKind::Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                             std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    uint32 spell = 0;
    uint32 botLow = 0;
    Player* const requester = request.nearPlayer;
    if (!services.config.serviceEnable || !services.config.serviceMax)
        why = "player services disabled";
    else if (!requester || !SafeRequester(requester, requester->GetMapId()) || requester->GetGroupInvite())
        why = "an available online PvE player without a group or invitation is required";
    else if (!PlayerServiceCatalog::ParseTarget(request.target, spell, botLow))
        why = "service target must be a learned recipe spell[:bot-counter] from the catalogue";
    else if (!request.capacity)
        why = "no participant capacity";
    else if (request.team && *request.team != requester->GetTeamId())
        why = "requester faction differs";
    else
    {
        auto state = std::make_unique<ServiceState>();
        for (ActivityCandidate const& candidate : services.candidates.Find(Query(requester, services.config)))
        {
            if (botLow && candidate.guid.GetCounter() != botLow)
                continue;
            Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
            if (!Nearby(bot, requester, services.config))
                continue;
            auto const recipe = Learned(bot, spell);
            if (!recipe)
                continue;
            state->spell = spell;
            state->item = recipe->itemId;
            state->skill = recipe->skillId;
            state->map = requester->GetMapId();
            plan.kind = Id();
            plan.gm = false;
            plan.requester = requester->GetGUID();
            plan.team = uint8(requester->GetTeamId());
            plan.minLevel = 1;
            plan.maxLevel = 60;
            plan.rally = ActivityWorld::PointOf(requester);
            plan.target = std::to_string(spell) + ":" + std::to_string(candidate.guid.GetCounter());
            plan.name = recipe->item;
            plan.wanted = plan.needed = 1;
            plan.leaderFixed = true;
            plan.candidates = {candidate};
            plan.decisions.clear();
            plan.state = std::move(state);
            return true;
        }
        why = "no nearby free PvE bot actually knows that supported recipe";
    }
    return false;
}

bool PlayerServiceKind::Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    ServiceState* const state = dynamic_cast<ServiceState*>(plan.state.get());
    Player* const requester = ObjectAccessor::FindConnectedPlayer(plan.requester);
    if (!services.config.serviceEnable || !services.config.serviceMax || !state || plan.candidates.size() != 1 ||
        plan.decisions.size() != 1 || plan.decisions[0] != 1 || !SafeRequester(requester, state->map) ||
        requester->GetGroupInvite())
        return false;
    ActivityCandidate const& candidate = plan.candidates.front();
    Player* const bot = ObjectAccessor::FindConnectedPlayer(candidate.guid);
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!ai || ai->GetCoaEpoch() != candidate.epoch || !ActivityCandidates::IsFree(bot) ||
        !Nearby(bot, requester, services.config) || !Learned(bot, state->spell))
        return false;
    activity.requester = plan.requester;
    activity.leader = candidate.guid;
    activity.target = plan.target;
    activity.name = plan.name;
    activity.minLevel = 1;
    activity.maxLevel = 60;
    state->formUntilMs = services.nowMs + ActivityThreshold::FORM_TIMEOUT_MS;
    activity.hardDeadlineMs = state->formUntilMs + uint64(services.config.serviceInviteSeconds) * 1000 +
                              uint64(services.config.serviceMinutes) * 60000;
    if (!Lease(candidate.guid.GetCounter(), {activity.id, candidate.epoch, plan.requester, state->spell,
                                              activity.hardDeadlineMs}, services.nowMs))
        return false;
    activity.Enroll(candidate.guid, candidate.epoch, true, candidate.level, services.nowUnix);
    activity.SetCenter(plan.rally, 10.0f);
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    activity.SetPhase(ActivityPhase::Forming, services.nowMs);
    activity.state = std::move(plan.state);
    return true;
}

void PlayerServiceKind::Tick(ActivityServices& services, Activity& activity)
{
    COA_ASSERT_WORLD_THREAD();
    ServiceState* const state = StateOf(activity);
    Player* const requester = Requester(activity);
    ActivityMember const* const member = activity.Leader();
    Player* const bot = member && member->In() ? ActivityWorld::FindMember(*member) : nullptr;
    if (!services.config.serviceEnable)
    {
        activity.Finish(ActivityOutcome::Disabled, "player services disabled");
        return;
    }
    if (!state || !SafeRequester(requester, state->map, activity.groupGuid) || !bot ||
        !Nearby(bot, requester, services.config) || !Learned(bot, state->spell))
    {
        activity.Finish(ActivityOutcome::Guarded, "service participant or learned recipe became unavailable");
        return;
    }
    Group* group = ServiceGroup(activity);
    if ((group && (bot->GetGroup() != group || group->GetMembersCount() != (state->accepted ? 2u : 1u))) ||
        (!group && (bot->GetGroup() || GET_PLAYERBOT_AI(bot)->GetMaster())))
    {
        activity.Finish(ActivityOutcome::Stopped, "service group ownership changed");
        return;
    }
    activity.SetTravel(ActivityTravel::None);
    activity.SetFlags(ACTIVITY_VIEW_QUIET);
    if (activity.phase == ActivityPhase::Forming)
    {
        if (services.nowMs >= state->formUntilMs)
        {
            activity.Finish(ActivityOutcome::Timeout, "service group could not form");
            return;
        }
        std::vector<ObjectGuid> added;
        std::vector<ObjectGuid> skipped;
        ObjectGuid groupGuid;
        ActivityOpResult const result = services.ops.FormGroup(activity.leader, {}, uint8(GROUP_LOOT),
                                                               uint8(ITEM_QUALITY_UNCOMMON), groupGuid, added, skipped);
        if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
            return;
        if (result != ActivityOpResult::Done || groupGuid.IsEmpty())
        {
            activity.Finish(ActivityOutcome::Stopped, "service group formation failed");
            return;
        }
        activity.groupGuid = groupGuid;
        activity.viewDirty = true;
        state->inviteUntilMs = services.nowMs + uint64(services.config.serviceInviteSeconds) * 1000;
        activity.SetPhase(ActivityPhase::Recruiting, services.nowMs);
        group = ServiceGroup(activity);
    }
    if (activity.phase == ActivityPhase::Recruiting)
    {
        if (services.nowMs >= state->inviteUntilMs)
        {
            activity.Finish(ActivityOutcome::Timeout, "service invitation expired");
            return;
        }
        if (state->invited && (!group || requester->GetGroupInvite() != group))
        {
            activity.Finish(ActivityOutcome::Stopped, "service invitation declined or cancelled");
            return;
        }
        if (!state->invited)
        {
            ActivityOpResult const result = services.ops.InvitePlayer(activity.leader, activity.requester);
            if (result == ActivityOpResult::Done)
            {
                state->invited = true;
                activity.invites[activity.requester] = state->inviteUntilMs;
                QueueLine(services, *state, "coa_act_service_invite",
                          "Accept my party invitation for %target. You choose any craft or trade; nothing is promised.",
                          {{"%target", activity.name}});
            }
            else if (result != ActivityOpResult::Deferred && result != ActivityOpResult::Refused)
            {
                activity.Finish(ActivityOutcome::Stopped, "service invitation failed");
                return;
            }
        }
    }
    if (activity.phase == ActivityPhase::Active)
    {
        ActivityMember const* const owner = activity.FindMember(activity.requester);
        if (!owner || !owner->In() || !group || requester->GetGroup() != group)
        {
            activity.Finish(ActivityOutcome::Stopped, "requester left the service party");
            return;
        }
        if (!state->leadership)
        {
            ActivityOpResult const result = services.ops.HandLeader(activity.groupGuid, activity.requester);
            if (result == ActivityOpResult::Deferred || result == ActivityOpResult::Refused)
                return;
            if (result != ActivityOpResult::Done)
            {
                activity.Finish(ActivityOutcome::Stopped, "service leadership could not pass to requester");
                return;
            }
            state->leadership = true;
        }
        activity.SetFollow(activity.requester);
        activity.SetCenter(ActivityWorld::PointOf(requester), 10.0f);
        if (services.nowMs >= state->workUntilMs)
            activity.Finish(ActivityOutcome::Timeout,
                            "service availability ended; no trade or craft completion claimed");
    }
    if (!activity.finishing)
        PostLine(services, activity, *state);
}

void PlayerServiceKind::OnMemberLeft(ActivityServices& /*services*/, Activity& activity, ActivityMember const& member)
{
    activity.Finish(member.isBot ? ActivityOutcome::TooFew : ActivityOutcome::Stopped,
                    member.isBot ? "crafter left" : "requester left");
}

bool PlayerServiceKind::Accepts(ActivityServices& services, Activity const& activity, Player* player)
{
    ServiceState const* const state = dynamic_cast<ServiceState const*>(activity.state.get());
    return services.config.serviceEnable && state && player && player->GetGUID() == activity.requester &&
           !activity.finishing && activity.phase == ActivityPhase::Recruiting && !state->accepted &&
           SafeRequester(player, state->map) && !activity.groupGuid.IsEmpty();
}

std::string PlayerServiceKind::Join(ActivityServices& services, Activity& activity, Player* player)
{
    bool const accepts = Accepts(services, activity, player);
    return ActivityWorld::Line(accepts ? "coa_act_service_pending" : "coa_act_service_private",
                               accepts ? "Accept the pending service party invitation." :
                                         "This service is reserved for its requester.");
}

bool PlayerServiceKind::OnPlayerJoined(ActivityServices& services, Activity& activity, ActivityMember const& member)
{
    ServiceState* const state = StateOf(activity);
    Player* const requester = Requester(activity);
    Group* const group = ServiceGroup(activity);
    if (!services.config.serviceEnable || !state || member.isBot || member.guid != activity.requester || !group ||
        activity.phase != ActivityPhase::Recruiting ||
        !EscortRules::CanAcceptInvitation(state->invited, state->accepted, services.nowMs, state->inviteUntilMs) ||
        !SafeRequester(requester, state->map, activity.groupGuid) || requester->GetGroup() != group)
    {
        activity.Finish(ActivityOutcome::Stopped, "late or invalid service acceptance");
        return true;
    }
    state->accepted = true;
    state->workUntilMs = services.nowMs + uint64(services.config.serviceMinutes) * 60000;
    activity.hardDeadlineMs = state->workUntilMs;
    activity.SetFollow(activity.requester);
    activity.SetPhase(ActivityPhase::Active, services.nowMs);
    QueueLine(services, *state, "coa_act_service_ready",
              "I know %target. Lead me locally; whisper spells, craft %item, craft ?, or trade using native commands.",
              {{"%target", activity.name}, {"%item", std::to_string(state->item)}});
    return true;
}

std::string PlayerServiceKind::Describe(Activity const& activity, uint64 nowMs) const
{
    ServiceState const* const state = dynamic_cast<ServiceState const*>(activity.state.get());
    if (!state)
        return "service state missing";
    uint64 const deadline = state->accepted ? state->workUntilMs : state->inviteUntilMs ? state->inviteUntilMs :
                                                                                     state->formUntilMs;
    return Acore::StringFormat("requester {}, learned recipe {}, item {}, skill {}, {}, {} s left; native trades only",
                               activity.requester.GetCounter(), state->spell, state->item, state->skill,
                               state->accepted ? "player-led service" : "awaiting consent",
                               deadline > nowMs ? (deadline - nowMs) / 1000 : 0);
}

std::unique_ptr<ActivityKind> CreatePlayerServiceKind() { return std::make_unique<PlayerServiceKind>(); }
