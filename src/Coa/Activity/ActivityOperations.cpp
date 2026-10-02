/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityOperations.h"
#include "ActivityKind.h"
#include "Channel.h"
#include "Creature.h"
#include "LootMgr.h"
#include "Opcodes.h"
#include "ChannelMgr.h"
#include "CoaChannels.h"
#include "CoaText.h"
#include "CoaThread.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "ActivityRegistry.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cctype>
#include <utility>

namespace
{
constexpr char const* LOGGER = "playerbots.activity";

// A bot session with its PlayerbotAI (random bots, altbots); never a real player, never a self-bot.
bool IsBot(Player* player)
{
    return player && player->GetSession() && player->GetSession()->IsBot() && GET_PLAYERBOT_AI(player);
}

ActivityOpResult Report(char const* operation, ObjectGuid guid, ActivityOpResult result)
{
    LOG_DEBUG(LOGGER, "activity op {} {}: {}", operation, guid.GetCounter(), ActivityOpResultName(result));
    return result;
}

// AcceptInvitationAction's steps for a bot just added to `leader`'s group, on the world thread: the master, then a
// strategy reset on the bot's own thread (AiFactory's activity block keeps follow, drops lfg and bg), then a
// non-full Reset that keeps rpgInfo, as ArenaGroupFormationOperation does. No greeting.
void MemberSteps(Player* member, Player* leader)
{
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(member);
    if (!ai)
        return;
    if (sRandomPlayerbotMgr.IsRandomBot(member))
        ai->SetMaster(leader);
    ai->SetCoaStrategiesDirty();
    ai->Reset();
}

// A channel named after the place its players stand in ("Zone - Elwynn Forest", "General - Westfall"). A bot stays
// on the ones of the zones it walked through (JoinBotChannels never leaves them), so a post needs the place's name.
bool PlaceBound(Channel const* channel)
{
    uint32 const id = channel->GetChannelId();
    return id && (id == sPlayerbotAIConfig.zoneChannelId || id == uint32(ChatChannelId::GENERAL) ||
                  id == uint32(ChatChannelId::LOCAL_DEFENSE));
}

// The names a place-bound channel of `zone` carries, best first: the zone's, then the area's when the bot stands in
// that zone (the CoA client names its zone channel after the area wherever the two differ, see JoinBotChannels).
std::vector<std::string> PlaceNames(AreaTableEntry const* zone, AreaTableEntry const* here, AreaTableEntry const* area)
{
    std::vector<std::string> names;
    if (!zone)
        return names;
    names.push_back(PlayerbotAI::GetLocalizedAreaName(zone));
    if (zone == here && area && area != zone)
        if (std::string areaName = PlayerbotAI::GetLocalizedAreaName(area); areaName != names.front())
            names.push_back(std::move(areaName));
    return names;
}

// The index of the first of `places` that `name` carries, or places.size().
std::size_t PlaceRank(std::string const& name, std::vector<std::string> const& places)
{
    for (std::size_t i = 0; i < places.size(); ++i)
        if (!places[i].empty() && name.find(places[i]) != std::string::npos)
            return i;
    return places.size();
}

// A channel whose name, lower-cased, starts with one of `prefixes` (lower case): the ones Chat.Channels lists.
bool Listed(Channel const* channel, std::vector<std::string> const& prefixes)
{
    std::string name = channel->GetName();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::any_of(prefixes.begin(), prefixes.end(), [&name](std::string const& prefix)
                       { return !prefix.empty() && name.rfind(prefix, 0) == 0; });
}

// The place-bound channels of the place `audience` stands in, the zone's name before the area's: listed by
// `prefixes`, held by `mgr` (the bot's team's manager: a channel of another team's is never joined), and really joined
// by the player (CoaChannels::Joined). The client leaves the channels of a zone it left, but a name that matches
// neither the zone nor the area the player stands in is not its place's channel and is never taken.
std::vector<Channel*> AudienceChannels(ChannelMgr* mgr, Player const* audience,
                                       std::vector<std::string> const& prefixes)
{
    AreaTableEntry const* const zone = sAreaTableStore.LookupEntry(audience->GetZoneId());
    AreaTableEntry const* const area = sAreaTableStore.LookupEntry(audience->GetAreaId());
    std::vector<std::string> const places = PlaceNames(zone, zone, area);

    std::vector<std::pair<std::size_t, Channel*>> found;
    for (Channel* const channel : CoaChannels::Joined(audience))
    {
        if (!channel || channel->GetName().empty() || !PlaceBound(channel) || !Listed(channel, prefixes))
            continue;
        if (mgr->GetChannel(channel->GetName(), nullptr, false) != channel)
            continue;
        if (std::size_t const rank = PlaceRank(channel->GetName(), places); rank < places.size())
            found.emplace_back(rank, channel);
    }

    std::stable_sort(found.begin(), found.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    std::vector<Channel*> channels;
    for (auto const& entry : found)
        channels.push_back(entry.second);
    return channels;
}
}  // namespace

char const* ActivityOpResultName(ActivityOpResult result)
{
    switch (result)
    {
        case ActivityOpResult::Done:
            return "done";
        case ActivityOpResult::Deferred:
            return "deferred";
        case ActivityOpResult::Refused:
            return "refused";
        case ActivityOpResult::Failed:
            return "failed";
        case ActivityOpResult::Gone:
            return "gone";
        default:
            return "unknown";
    }
}

void ActivityOperations::BeginTick(uint32 maxOps, std::vector<Player*> const* players)
{
    budget = maxOps;
    used = 0;
    realPlayers = players;
}

bool ActivityOperations::Spend(uint32 amount)
{
    if (used + amount > budget)
        return false;
    used += amount;
    return true;
}

ActivityOpResult ActivityOperations::FormGroup(ObjectGuid leaderGuid, std::vector<ObjectGuid> const& members,
                                               uint8 lootMethod, uint8 lootThreshold, ObjectGuid& groupGuid,
                                               std::vector<ObjectGuid>& added, std::vector<ObjectGuid>& skipped)
{
    COA_ASSERT_WORLD_THREAD();
    added.clear();
    skipped.clear();
    if (!Spend())
        return Report("form group", leaderGuid, ActivityOpResult::Deferred);

    Player* const leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
    if (!IsBot(leader) || !leader->IsInWorld() || leader->GetGroup())
        return Report("form group", leaderGuid, ActivityOpResult::Failed);

    Group* const group = new Group();
    if (!group->Create(leader))
    {
        delete group;
        return Report("form group", leaderGuid, ActivityOpResult::Failed);
    }
    sGroupMgr->AddGroup(group);
    group->SetLootMethod(LootMethod(lootMethod));
    group->SetLootThreshold(ItemQualities(lootThreshold));
    groupGuid = group->GetGUID();

    bool budgetOut = false;
    for (ObjectGuid const& memberGuid : members)
    {
        if (budgetOut)
        {
            skipped.push_back(memberGuid);
            continue;
        }

        Player* const member = ObjectAccessor::FindConnectedPlayer(memberGuid);
        if (!IsBot(member) || !member->IsInWorld() || member == leader)
            continue;
        // Grouped since it was chosen (a player's invite): left alone, the member pass sees it.
        if (member->GetGroup())
            continue;
        if (!Spend())
        {
            skipped.push_back(memberGuid);
            budgetOut = true;
            continue;
        }

        if (!group->isRaidGroup() && group->GetMembersCount() >= 5)
            group->ConvertToRaid();
        if (!group->AddMember(member))
            continue;

        MemberSteps(member, leader);
        added.push_back(memberGuid);
    }

    group->SendUpdate();
    LOG_DEBUG(LOGGER, "activity op form group {}: group {}, {} added, {} skipped", leaderGuid.GetCounter(),
              groupGuid.GetCounter(), added.size(), skipped.size());
    return ActivityOpResult::Done;
}

ActivityOpResult ActivityOperations::AddMember(ObjectGuid groupGuid, ObjectGuid leaderGuid, ObjectGuid memberGuid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return Report("add member", memberGuid, ActivityOpResult::Deferred);

    Group* const group = groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
    Player* const leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
    if (!group || !leader || !group->IsLeader(leaderGuid))
        return Report("add member", memberGuid, ActivityOpResult::Failed);

    Player* const member = ObjectAccessor::FindConnectedPlayer(memberGuid);
    if (!member || !member->IsInWorld())
        return Report("add member", memberGuid, ActivityOpResult::Gone);
    if (!IsBot(member) || member->GetGroup())
        return Report("add member", memberGuid, ActivityOpResult::Failed);

    if (!group->isRaidGroup() && group->GetMembersCount() >= 5)
        group->ConvertToRaid();
    if (!group->AddMember(member))
        return Report("add member", memberGuid, ActivityOpResult::Failed);

    MemberSteps(member, leader);
    group->SendUpdate();
    return Report("add member", memberGuid, ActivityOpResult::Done);
}

ActivityOpResult ActivityOperations::InvitePlayer(ObjectGuid leaderGuid, ObjectGuid playerGuid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return Report("invite player", playerGuid, ActivityOpResult::Deferred);

    Player* const leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
    if (!IsBot(leader) || !leader->IsInWorld())
        return Report("invite player", playerGuid, ActivityOpResult::Failed);

    Player* const player = ObjectAccessor::FindConnectedPlayer(playerGuid);
    if (!player || !player->IsInWorld() || !player->GetSession())
        return Report("invite player", playerGuid, ActivityOpResult::Gone);
    if (player->GetSession()->IsBot())
        return Report("invite player", playerGuid, ActivityOpResult::Failed);
    if (player->GetGroup() || player->GetGroupInvite())
        return Report("invite player", playerGuid, ActivityOpResult::Refused);

    if (Group* const group = leader->GetGroup())
        if (!group->isRaidGroup() && group->GetMembersCount() >= 5)
            group->ConvertToRaid();

    // As InviteToGroupAction::Invite: the leader's session sends an ordinary invitation the player accepts or not.
    WorldPacket packet;
    packet << player->GetName();
    packet << uint32(0);
    leader->GetSession()->HandleGroupInviteOpcode(packet);

    // The core refused it (ignored, other faction, ...): nothing is pending for the player.
    if (!player->GetGroupInvite())
        return Report("invite player", playerGuid, ActivityOpResult::Refused);
    return Report("invite player", playerGuid, ActivityOpResult::Done);
}

ActivityOpResult ActivityOperations::PostToChannel(ObjectGuid botGuid, ActivityChatTarget target,
                                                   std::string const& text, ObjectGuid whisperTo)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return Report("post", botGuid, ActivityOpResult::Deferred);

    Player* const bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    if (!IsBot(bot) || !bot->IsInWorld())
        return Report("post", botGuid, ActivityOpResult::Gone);
    if (text.empty())
        return Report("post", botGuid, ActivityOpResult::Failed);
    if (bot->IsInCombat() && target != ActivityChatTarget::Whisper && target != ActivityChatTarget::Party)
        return Report("post", botGuid, ActivityOpResult::Refused);

    std::string line = text;
    CoaText::CutUtf8Bytes(line, 255);
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);

    bool said = false;
    switch (target)
    {
        case ActivityChatTarget::Say:
            said = ai->Say(line);
            break;
        case ActivityChatTarget::Yell:
            said = ai->Yell(line);
            break;
        case ActivityChatTarget::Party:
            if (!bot->GetGroup())
                return Report("post", botGuid, ActivityOpResult::Failed);
            said = ai->SayToParty(line);
            break;
        case ActivityChatTarget::Zone:
            said = ai->SayToChannel(line, ChatChannelId::GENERAL);
            break;
        case ActivityChatTarget::World:
            said = ai->SayToWorld(line);
            break;
        case ActivityChatTarget::Whisper:
        {
            Player* const receiver = ObjectAccessor::FindConnectedPlayer(whisperTo);
            if (!receiver || !receiver->IsInWorld())
                return Report("post", botGuid, ActivityOpResult::Gone);
            bot->Whisper(line, LANG_UNIVERSAL, receiver);
            said = true;
            break;
        }
        default:
            break;
    }
    return Report("post", botGuid, said ? ActivityOpResult::Done : ActivityOpResult::Failed);
}

bool ActivityOperations::EnterChannel(Player* bot, Channel* channel)
{
    if (CoaChannels::IsOn(bot, channel))
        return true;

    // The same call JoinBotChannels makes for the channels of a zone; a banned or password-protected channel leaves
    // the bot outside.
    channel->JoinChannel(bot, "");
    if (!CoaChannels::IsOn(bot, channel))
        return false;

    ChannelJoins& entry = joined[bot->GetGUID()];
    uint64 const epoch = GET_PLAYERBOT_AI(bot)->GetCoaEpoch();
    if (entry.epoch != epoch)
    {
        entry.epoch = epoch;  // an earlier life's channels went with its session
        entry.names.clear();
    }
    entry.names.push_back(channel->GetName());
    return true;
}

ActivityOpResult ActivityOperations::PostToChannels(ObjectGuid botGuid, std::vector<std::string> const& prefixes,
                                                    std::string const& text, uint32 zoneId, ObjectGuid audienceGuid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return Report("post channels", botGuid, ActivityOpResult::Deferred);

    Player* const bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    if (!IsBot(bot) || !bot->IsInWorld())
        return Report("post channels", botGuid, ActivityOpResult::Gone);
    if (text.empty())
        return Report("post channels", botGuid, ActivityOpResult::Failed);
    if (bot->IsInCombat())
        return Report("post channels", botGuid, ActivityOpResult::Refused);

    ChannelMgr* const mgr = ChannelMgr::forTeam(bot->GetTeamId());
    if (!mgr)
        return Report("post channels", botGuid, ActivityOpResult::Failed);

    std::string line = text;
    CoaText::CutUtf8Bytes(line, 255);
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(bot);

    // The player the post is for: the place channels it is on come first, whether the bot is on them or not.
    Player* const audience = audienceGuid.IsEmpty() ? nullptr : ObjectAccessor::FindConnectedPlayer(audienceGuid);
    std::vector<Channel*> const reach = audience && audience != bot && audience->IsInWorld()
                                            ? AudienceChannels(mgr, audience, prefixes)
                                            : std::vector<Channel*>();

    // Otherwise the places a place-bound channel of the bot's own may name: the activity's zone, else (no zone given,
    // or the bot is on no channel of it) the zone the bot stands in. Never another place whose channel the bot is
    // still on.
    AreaTableEntry const* const here = ai->GetCurrentZone();
    AreaTableEntry const* const area = ai->GetCurrentArea();
    AreaTableEntry const* const target = zoneId ? sAreaTableStore.LookupEntry(zoneId) : here;
    std::vector<std::string> const wanted = PlaceNames(target ? target : here, here, area);
    std::vector<std::string> const fallback =
        target && target != here ? PlaceNames(here, here, area) : std::vector<std::string>();

    // "General - Elwynn Forest" matches the prefix "general". Only channels the bot is really on (CoaChannels::IsOn:
    // Player::IsInChannel compares ids, and every "Zone - <place>" channel has the same one).
    std::vector<std::pair<std::size_t, Channel*>> placed;
    std::vector<std::pair<std::size_t, Channel*>> placedHere;
    std::vector<Channel*> placeless;
    for (auto const& [key, channel] : mgr->GetChannels())
    {
        if (!channel || channel->GetName().empty())
            continue;
        if (!Listed(channel, prefixes) || !CoaChannels::IsOn(bot, channel))
            continue;
        if (!PlaceBound(channel))
            placeless.push_back(channel);
        else if (std::size_t const rank = PlaceRank(channel->GetName(), wanted); rank < wanted.size())
            placed.emplace_back(rank, channel);
        else if (std::size_t const rankHere = PlaceRank(channel->GetName(), fallback); rankHere < fallback.size())
            placedHere.emplace_back(rankHere, channel);
    }

    // The place's own channels first (the zone's name before the area's), then the listed channels of no place.
    std::vector<std::pair<std::size_t, Channel*>>& chosen = placed.empty() ? placedHere : placed;
    std::stable_sort(chosen.begin(), chosen.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

    std::string spoken;
    uint32 said = 0;
    uint32 joinedNow = 0;
    auto speak = [&](Channel* channel)
    {
        channel->Say(bot->GetGUID(), line, LANG_UNIVERSAL);
        ai->LogChat("tells channel " + channel->GetName(), line);
        spoken += (said ? ", " : "") + channel->GetName();
        ++said;
    };

    // The audience's channels: joined now when the bot is not on them. A channel that refuses the bot is skipped.
    for (Channel* const channel : reach)
    {
        if (said >= 2)
            break;
        bool const wasOn = CoaChannels::IsOn(bot, channel);
        if (!EnterChannel(bot, channel))
            continue;
        joinedNow += wasOn ? 0 : 1;
        speak(channel);
    }
    // None reached (no audience, none of its place channels listed or joinable): the bot's own place channels.
    if (!said)
        for (auto const& entry : chosen)
        {
            if (said >= 2)
                break;
            speak(entry.second);
        }
    for (Channel* const channel : placeless)
    {
        if (said >= 2)
            break;
        speak(channel);
    }

    if (!said)
        return Report("post channels", botGuid, ActivityOpResult::Failed);
    LOG_DEBUG(LOGGER, "activity op post channels {} (zone {}, audience {}, {} of its channels, {} joined): {}",
              botGuid.GetCounter(), zoneId, audience ? audience->GetGUID().GetCounter() : 0, reach.size(), joinedNow,
              spoken);
    return ActivityOpResult::Done;
}

void ActivityOperations::LeaveActivityChannels(ObjectGuid botGuid)
{
    COA_ASSERT_WORLD_THREAD();
    auto const it = joined.find(botGuid);
    if (it == joined.end())
        return;
    ChannelJoins const entry = std::move(it->second);
    joined.erase(it);

    Player* const bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    ChannelMgr* const mgr = bot ? ChannelMgr::forTeam(bot->GetTeamId()) : nullptr;
    if (!ai || !mgr || ai->GetCoaEpoch() != entry.epoch)
        return;

    // The channel of the place the bot stands in now stays: JoinBotChannels put it there (or would have, when the bot
    // walked in while it was on this one already) and nothing else brings it back. GetCurrentZone needs the bot's map,
    // so a bot in a far teleport or loading screen has no place: every recorded channel is left, and JoinBotChannels
    // joins the landing zone's channel on arrival.
    std::vector<std::string> places;
    if (bot->IsInWorld() && !bot->IsDuringRemoveFromWorld())
    {
        AreaTableEntry const* const here = ai->GetCurrentZone();
        places = PlaceNames(here, here, ai->GetCurrentArea());
    }
    uint32 left = 0;
    for (std::string const& name : entry.names)
    {
        Channel* const channel = mgr->GetChannel(name, nullptr, false);
        if (!channel || !CoaChannels::IsOn(bot, channel) || PlaceRank(channel->GetName(), places) < places.size())
            continue;
        channel->LeaveChannel(bot);
        ++left;
    }
    LOG_DEBUG(LOGGER, "activity op leave channels {}: {} of {} left", botGuid.GetCounter(), left, entry.names.size());
}

ActivityOpResult ActivityOperations::SetIcon(ObjectGuid setter, uint8 icon, ObjectGuid target)
{
    COA_ASSERT_WORLD_THREAD();
    ++used;
    Player* const player = ObjectAccessor::FindConnectedPlayer(setter);
    Group* const group = player ? player->GetGroup() : nullptr;
    if (!group)
        return Report("set icon", setter, ActivityOpResult::Failed);
    group->SetTargetIcon(icon, setter, target);
    return Report("set icon", setter, ActivityOpResult::Done);
}

ActivityOpResult ActivityOperations::HandLeader(ObjectGuid groupGuid, ObjectGuid newLeader)
{
    COA_ASSERT_WORLD_THREAD();
    ++used;
    Group* const group = groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
    if (!group || !group->IsMember(newLeader) || !ObjectAccessor::FindConnectedPlayer(newLeader))
        return Report("hand leader", newLeader, ActivityOpResult::Failed);
    group->ChangeLeader(newLeader);
    group->SendUpdate();
    return Report("hand leader", newLeader, ActivityOpResult::Done);
}

ActivityOpResult ActivityOperations::SetLoot(ObjectGuid groupGuid, ObjectGuid leaderGuid, uint8 lootMethod,
                                            uint8 threshold, ObjectGuid looterGuid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return ActivityOpResult::Deferred;
    Player* const leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
    Group* const group = leader ? leader->GetGroup() : nullptr;
    if (!IsBot(leader) || !group || group->GetGUID() != groupGuid || group->GetLeaderGUID() != leaderGuid ||
        lootMethod > NEED_BEFORE_GREED || threshold > ITEM_QUALITY_ARTIFACT ||
        (!looterGuid.IsEmpty() && !group->IsMember(looterGuid)))
        return ActivityOpResult::Failed;
    group->SetLootMethod(LootMethod(lootMethod));
    group->SetLootThreshold(ItemQualities(threshold));
    group->SetLooterGuid(looterGuid.IsEmpty() ? leaderGuid : looterGuid);
    group->SetMasterLooterGuid(lootMethod == MASTER_LOOT ?
        (looterGuid.IsEmpty() ? leaderGuid : looterGuid) : ObjectGuid::Empty);
    group->SendUpdate();
    return ActivityOpResult::Done;
}

ActivityOpResult ActivityOperations::ConvertToRaid(ObjectGuid groupGuid, ObjectGuid leaderGuid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return ActivityOpResult::Deferred;
    Player* const leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
    Group* const group = leader ? leader->GetGroup() : nullptr;
    if (!IsBot(leader) || !group || group->GetGUID() != groupGuid || group->GetLeaderGUID() != leaderGuid)
        return ActivityOpResult::Failed;
    if (!group->isRaidGroup())
        group->ConvertToRaid();
    group->SendUpdate();
    return ActivityOpResult::Done;
}

ActivityOpResult ActivityOperations::EnterPortal(ObjectGuid botGuid, uint32 triggerId)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return ActivityOpResult::Deferred;
    Player* const bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    auto const row = ActivityRegistry::Find(botGuid.GetCounter());
    PlayerbotAI* const ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    AreaTrigger const* const trigger = sObjectMgr->GetAreaTrigger(triggerId);
    if (!IsBot(bot) || !sRandomPlayerbotMgr.IsRandomBot(bot) || !bot->IsInWorld() || !bot->IsAlive() ||
        bot->IsInCombat() || bot->IsBeingTeleported() || bot->IsInFlight() || bot->GetVehicle() || bot->duel ||
        !row || row->kind != ActivityKindId::PugRaid || !ai || row->epoch != ai->GetCoaEpoch() ||
        !trigger || !sObjectMgr->GetAreaTriggerTeleport(triggerId) || !bot->IsInAreaTriggerRadius(trigger))
        return ActivityOpResult::Refused;
    WorldPacket packet(CMSG_AREATRIGGER, 4);
    packet << triggerId;
    bot->GetSession()->HandleAreaTriggerOpcode(packet);
    return bot->IsBeingTeleported() ? ActivityOpResult::Done : ActivityOpResult::Refused;
}

ActivityOpResult ActivityOperations::GiveMasterLoot(ObjectGuid leaderGuid, ObjectGuid source, uint8 slot,
                                                   ObjectGuid winnerGuid)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return ActivityOpResult::Deferred;
    Player* const leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
    Player* const winner = ObjectAccessor::FindConnectedPlayer(winnerGuid);
    Group* const group = leader ? leader->GetGroup() : nullptr;
    if (!IsBot(leader) || !leader->IsInWorld() || !winner || !winner->IsInWorld() || !group ||
        group->GetLootMethod() != MASTER_LOOT || group->GetMasterLooterGuid() != leaderGuid ||
        group->GetLeaderGUID() != leaderGuid || !group->IsMember(winnerGuid) ||
        leader->GetMap() != winner->GetMap())
        return ActivityOpResult::Refused;
    Creature* const corpse = leader->GetMap()->GetCreature(source);
    if (!corpse || corpse->IsAlive() || !leader->IsWithinDistInMap(corpse, INTERACTION_DISTANCE) ||
        slot >= corpse->loot.items.size() || corpse->loot.items[slot].is_looted)
        return ActivityOpResult::Refused;
    leader->SendLoot(source, LOOT_CORPSE);
    if (leader->GetLootGUID() != source)
        return ActivityOpResult::Refused;
    WorldPacket packet(CMSG_LOOT_MASTER_GIVE, 17);
    packet << source << slot << winnerGuid;
    leader->GetSession()->HandleLootMasterGiveOpcode(packet);
    bool const given = corpse->loot.items[slot].is_looted;
    leader->GetSession()->DoLootRelease(source);
    return given ? ActivityOpResult::Done : ActivityOpResult::Refused;
}

ActivityOpResult ActivityOperations::Teleport(ObjectGuid botGuid, ActivityPoint const& dest, bool dead)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return Report("teleport", botGuid, ActivityOpResult::Deferred);

    Player* const bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    if (!bot)
        return Report("teleport", botGuid, ActivityOpResult::Gone);
    if (!IsBot(bot) || !dest.set)
        return Report("teleport", botGuid, ActivityOpResult::Failed);
    if (!bot->IsInWorld() || bot->IsDuringRemoveFromWorld() || (!dead && !bot->IsAlive()) || bot->IsInCombat() ||
        bot->IsBeingTeleported() || bot->IsInFlight())
        return Report("teleport", botGuid, ActivityOpResult::Refused);

    // The visibility rule (RandomTeleport's HasPlayerNearby(150), on both ends).
    if (realPlayers && (ActivityWorld::RealPlayerNear(*realPlayers, ActivityWorld::PointOf(bot),
                                                      ActivityThreshold::TELEPORT_UNSEEN_YARDS) ||
                        ActivityWorld::RealPlayerNear(*realPlayers, dest, ActivityThreshold::TELEPORT_UNSEEN_YARDS)))
        return Report("teleport", botGuid, ActivityOpResult::Refused);

    bot->GetMotionMaster()->Clear();
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    if (!bot->TeleportTo(dest.mapId, dest.x, dest.y, dest.z, bot->GetOrientation()))
        return Report("teleport", botGuid, ActivityOpResult::Failed);
    return Report("teleport", botGuid, ActivityOpResult::Done);
}

ActivityOpResult ActivityOperations::ApplyWarMode(ObjectGuid botGuid, uint32& prevRuleset)
{
    return ApplyRuleset(botGuid, ActivityThreshold::SPELL_WAR_MODE, prevRuleset);
}

ActivityOpResult ActivityOperations::ApplyRuleset(ObjectGuid botGuid, uint32 spellId, uint32& prevRuleset)
{
    COA_ASSERT_WORLD_THREAD();
    using namespace ActivityThreshold;
    ++used;
    prevRuleset = 0;
    Player* const bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    if (!bot || !bot->IsInWorld())
        return ActivityOpResult::Gone;
    if (!IsBot(bot) || !sRandomPlayerbotMgr.IsRandomBot(bot) || bot->HasAura(SPELL_MERCENARY) ||
        (spellId != SPELL_WAR_MODE && spellId != SPELL_HIGH_RISK))
        return ActivityOpResult::Failed;
    uint32 const other = spellId == SPELL_WAR_MODE ? SPELL_HIGH_RISK : SPELL_WAR_MODE;
    if (bot->HasAura(spellId) && !bot->HasAura(SPELL_PVE) && !bot->HasAura(other) && bot->IsPvP())
        return ActivityOpResult::Done;
    uint32 const previous = bot->HasAura(SPELL_HIGH_RISK) ? SPELL_HIGH_RISK :
                            bot->HasAura(SPELL_WAR_MODE) ? SPELL_WAR_MODE :
                            bot->HasAura(SPELL_PVE) ? SPELL_PVE : RULESET_NONE;
    // High bit records a pre-existing PvP flag without changing legacy stored spell IDs.
    prevRuleset = previous | (bot->IsPvP() ? 0x80000000u : 0u);
    for (uint32 aura : {SPELL_HIGH_RISK, SPELL_WAR_MODE, SPELL_PVE, SPELL_MERCENARY})
        bot->RemoveAurasDueToSpell(aura);
    SpellCastResult const cast = bot->CastSpell(bot, spellId, true);
    bot->SetPlayerFlag(PLAYER_FLAGS_IN_PVP);
    bot->UpdatePvP(true, true);
    bool const applied = bot->HasAura(spellId) && !bot->HasAura(other) && !bot->HasAura(SPELL_PVE) &&
                         !bot->HasAura(SPELL_MERCENARY) && bot->IsPvP();
    if (!applied)
    {
        SpellInfo const* const info = sSpellMgr->GetSpellInfo(spellId);
        LOG_WARN(LOGGER, "ruleset application failed for bot {}: desired {}, cast result {}, explicit mask {}, "
            "first targets {}/{}, desired aura {}, PvE {}, War {}, High-Risk {}, PvP {}",
            bot->GetGUID().GetCounter(), spellId, uint32(cast), info ? info->GetExplicitTargetMask() : 0,
            info ? info->Effects[0].TargetA.GetTarget() : 0, info ? info->Effects[0].TargetB.GetTarget() : 0,
            bot->HasAura(spellId), bot->HasAura(SPELL_PVE), bot->HasAura(SPELL_WAR_MODE),
            bot->HasAura(SPELL_HIGH_RISK), bot->IsPvP());
    }
    return applied ? ActivityOpResult::Done : ActivityOpResult::Failed;
}

ActivityOpResult ActivityOperations::RestoreRuleset(ObjectGuid botGuid, uint32 prevRuleset)
{
    COA_ASSERT_WORLD_THREAD();
    using namespace ActivityThreshold;
    ++used;
    if (!prevRuleset)
        return ActivityOpResult::Done;
    Player* const bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    if (!bot || !bot->IsInWorld())
        return ActivityOpResult::Gone;
    if (!IsBot(bot))
        return ActivityOpResult::Failed;
    uint32 const spell = prevRuleset & 0x7FFFFFFFu;
    if (spell != RULESET_NONE && spell != SPELL_PVE && spell != SPELL_WAR_MODE && spell != SPELL_HIGH_RISK)
        return ActivityOpResult::Failed;
    for (uint32 aura : {SPELL_HIGH_RISK, SPELL_WAR_MODE, SPELL_PVE, SPELL_MERCENARY})
        bot->RemoveAurasDueToSpell(aura);
    if (spell != RULESET_NONE)
        bot->CastSpell(bot, spell, true);
    bool const pvp = (prevRuleset & 0x80000000u) || spell == SPELL_WAR_MODE || spell == SPELL_HIGH_RISK;
    if (pvp)
        bot->SetPlayerFlag(PLAYER_FLAGS_IN_PVP);
    else
        bot->RemovePlayerFlag(PLAYER_FLAGS_IN_PVP);
    bot->UpdatePvP(pvp, true);
    bool const restored = (spell == RULESET_NONE || bot->HasAura(spell)) && bot->IsPvP() == pvp &&
        !bot->HasAura(SPELL_MERCENARY) &&
        (spell == SPELL_PVE || !bot->HasAura(SPELL_PVE)) &&
        (spell == SPELL_WAR_MODE || !bot->HasAura(SPELL_WAR_MODE)) &&
        (spell == SPELL_HIGH_RISK || !bot->HasAura(SPELL_HIGH_RISK));
    return restored ? ActivityOpResult::Done : ActivityOpResult::Failed;
}

ActivityOpResult ActivityOperations::Revive(ObjectGuid botGuid, ActivityPoint const* rally)
{
    COA_ASSERT_WORLD_THREAD();
    if (!Spend())
        return Report("revive", botGuid, ActivityOpResult::Deferred);

    Player* const bot = ObjectAccessor::FindConnectedPlayer(botGuid);
    if (!bot)
        return Report("revive", botGuid, ActivityOpResult::Gone);
    if (!IsBot(bot))
        return Report("revive", botGuid, ActivityOpResult::Failed);
    if (!bot->IsInWorld() || bot->IsDuringRemoveFromWorld() || bot->IsBeingTeleported())
        return Report("revive", botGuid, ActivityOpResult::Refused);
    if (bot->IsAlive())
        return Report("revive", botGuid, ActivityOpResult::Done);

    bot->ResurrectPlayer(0.5f);
    bot->SpawnCorpseBones();
    // Alive now: the ordinary teleport rule applies; refused or deferred, it walks back from where it rose.
    if (rally && rally->set)
    {
        ActivityPoint const landing = ActivityWorld::GroundNear(*rally, 5.0f);
        if (landing.set)
            Teleport(botGuid, landing, false);
    }
    return Report("revive", botGuid, ActivityOpResult::Done);
}

ActivityOpResult ActivityOperations::Leave(ObjectGuid groupGuid, ObjectGuid botGuid)
{
    COA_ASSERT_WORLD_THREAD();
    ++used;
    Group* const group = groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
    if (!group || !group->IsMember(botGuid))
        return Report("leave", botGuid, ActivityOpResult::Failed);

    // Never a real player. An offline bot (a member that logged out) is removed all the same.
    Player* const player = ObjectAccessor::FindConnectedPlayer(botGuid);
    if (player && !IsBot(player))
        return Report("leave", botGuid, ActivityOpResult::Failed);

    if (group->GetLootMethod() == MASTER_LOOT && group->GetMasterLooterGuid() == botGuid)
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
            if (Player* const member = ObjectAccessor::FindConnectedPlayer(slot.guid);
                member && member->IsInWorld() && member->GetSession() && !member->GetSession()->IsBot())
            {
                group->SetMasterLooterGuid(member->GetGUID());
                group->SendUpdate();
                break;
            }

    group->RemoveMember(botGuid);
    return Report("leave", botGuid, ActivityOpResult::Done);
}

ActivityOpResult ActivityOperations::Disband(ObjectGuid groupGuid)
{
    COA_ASSERT_WORLD_THREAD();
    ++used;
    Group* group = groupGuid.IsEmpty() ? nullptr : sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
    if (!group)
        return Report("disband", groupGuid, ActivityOpResult::Done);

    // Only random bots go: the director enrolls no other bot. Everyone else stays, online or not: real players,
    // self-bots, and a player's own altbots and addclass bots (bot sessions too, but not the activity's).
    Player* firstReal = nullptr;
    bool anyKept = false;
    bool leaderGoes = false;
    std::vector<ObjectGuid> bots;
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        Player* const member = ObjectAccessor::FindConnectedPlayer(slot.guid);
        bool const randomBot = member ? sRandomPlayerbotMgr.IsRandomBot(member)
                                      : sRandomPlayerbotMgr.IsRandomBot(slot.guid.GetCounter());
        if (randomBot)
        {
            bots.push_back(slot.guid);
            leaderGoes = leaderGoes || group->IsLeader(slot.guid);
            continue;
        }
        anyKept = true;
        if (member && member->IsInWorld() && !firstReal && member->GetSession() && !member->GetSession()->IsBot())
            firstReal = member;
    }

    if (!anyKept)
    {
        group->Disband();
        return Report("disband", groupGuid, ActivityOpResult::Done);
    }

    // The group stays: the lead to a real player first (while a random bot or nobody online leads), then every
    // random bot out.
    if (firstReal && !group->IsLeader(firstReal->GetGUID()) &&
        (leaderGoes || !ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID())))
    {
        group->ChangeLeader(firstReal->GetGUID());
        group->SendUpdate();
    }
    if (firstReal && group->GetLootMethod() == MASTER_LOOT &&
        std::find(bots.begin(), bots.end(), group->GetMasterLooterGuid()) != bots.end())
    {
        group->SetMasterLooterGuid(firstReal->GetGUID());
        group->SendUpdate();
    }
    for (ObjectGuid const& bot : bots)
    {
        // RemoveMember may dissolve the group (two members left): look it up again every time.
        group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter());
        if (!group)
            break;
        group->RemoveMember(bot);
    }
    return Report("disband", groupGuid, ActivityOpResult::Done);
}
