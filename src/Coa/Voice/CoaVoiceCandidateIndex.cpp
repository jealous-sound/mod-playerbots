/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaVoiceCandidateIndex.h"
#include "CoaThread.h"
#include "NameMatch.h"
#include "Player.h"
#include "Playerbots.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

namespace
{
std::vector<uint32> const NoBots;

std::string_view FirstName(std::string const& name)
{
    std::string_view const view(name);
    return view.substr(0, view.find(' '));
}
}  // namespace

void CoaVoiceCandidateIndex::Rebuild(uint64 nowMs)
{
    COA_ASSERT_WORLD_THREAD();

    bots.clear();
    byZone.clear();
    byGuild.clear();
    byGuid.clear();

    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin(); it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        Add(it->second, true);

    // Every real player's own bots. A self-bot player has a PlayerbotMgr as well, so its alts are kept too;
    // bots never have one.
    for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* const player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            continue;

        if (PlayerbotMgr* const mgr = GET_PLAYERBOT_MGR(player))
            for (auto it = mgr->GetPlayerBotsBegin(); it != mgr->GetPlayerBotsEnd(); ++it)
                Add(it->second, false);
    }

    // Forget the names of bots long gone, without folding the online ones again.
    if (nameCache.size() > 2 * bots.size() + 1024)
        std::erase_if(nameCache, [this](auto const& entry) { return !byGuid.contains(entry.first); });

    builtMs = nowMs;
}

void CoaVoiceCandidateIndex::Add(Player* bot, bool fromRandomHolder)
{
    if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
        return;

    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || byGuid.contains(bot->GetGUID()))
        return;

    CoaVoiceIndexedBot entry;
    entry.guid = bot->GetGUID();

    auto& cached = nameCache[entry.guid];
    if (cached.first != bot->GetName() || cached.second.empty())
    {
        cached.first = bot->GetName();
        cached.second = NameMatch::Fold(FirstName(cached.first));
    }
    entry.name = cached.first;
    entry.nameKey = cached.second;

    entry.mapId = bot->GetMapId();
    entry.instanceId = bot->GetInstanceId();
    entry.zoneId = bot->GetZoneId();
    entry.guildId = bot->GetGuildId();
    entry.teamId = uint8(bot->GetTeamId());
    if (Player* const master = botAI->GetMaster())
        entry.masterGuid = master->GetGUID();
    entry.hasRealMaster = botAI->HasGameClientMaster();
    entry.randomBot = fromRandomHolder && sRandomPlayerbotMgr.IsRandomBot(bot);

    uint32 const index = uint32(bots.size());
    byZone[ZoneKey(entry.mapId, entry.zoneId)].push_back(index);
    if (entry.guildId)
        byGuild[entry.guildId].push_back(index);
    byGuid.emplace(entry.guid, index);
    bots.push_back(std::move(entry));
}

std::vector<uint32> const& CoaVoiceCandidateIndex::InZone(uint32 mapId, uint32 zoneId) const
{
    auto const it = byZone.find(ZoneKey(mapId, zoneId));
    return it != byZone.end() ? it->second : NoBots;
}

std::vector<uint32> const& CoaVoiceCandidateIndex::InGuild(uint32 guildId) const
{
    auto const it = byGuild.find(guildId);
    return it != byGuild.end() ? it->second : NoBots;
}

CoaVoiceIndexedBot const* CoaVoiceCandidateIndex::Find(ObjectGuid guid) const
{
    auto const it = byGuid.find(guid);
    return it != byGuid.end() ? &bots[it->second] : nullptr;
}

void CoaVoiceCandidateIndex::Clear()
{
    builtMs = 0;
    bots.clear();
    byZone.clear();
    byGuild.clear();
    byGuid.clear();
    nameCache.clear();
}
