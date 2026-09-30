/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICECANDIDATEINDEX_H
#define PLAYERBOTS_COAVOICECANDIDATEINDEX_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <unordered_map>
#include <vector>

class Player;

// One online bot as the index last saw it. Plain values: the director looks the Player up again before use.
struct CoaVoiceIndexedBot
{
    ObjectGuid guid;
    std::string name;     // as the core holds it
    std::string nameKey;  // NameMatch::Fold of the first name: random bots carry a surname ("Kegarink Bot")
    uint32 mapId = 0;
    uint32 instanceId = 0;
    uint32 zoneId = 0;
    uint32 guildId = 0;
    uint8 teamId = 0;
    ObjectGuid masterGuid;       // its master, empty when none
    bool hasRealMaster = false;  // PlayerbotAI::HasGameClientMaster: answers its master only
    bool randomBot = false;
};

// Who could hear what, rebuilt every CoaVoice.IndexRefreshSeconds on the world thread from the random bots and
// every real player's alt bots, so no chat message ever scans all bots. Per message the director reads one
// bucket (the speaker's zone, or the guild) and checks each entry against the live Player. An entry may be up
// to one period stale: a bot that just arrived is missed, a bot that left fails the live check.
class CoaVoiceCandidateIndex
{
public:
    // World thread. Walks sRandomPlayerbotMgr's bots and, for each real player's session, its PlayerbotMgr's
    // bots; keeps the ones in world with a PlayerbotAI. Names are folded once per GUID and cached across
    // rebuilds.
    void Rebuild(uint64 nowMs);

    bool IsStale(uint64 nowMs, uint32 refreshMs) const { return !builtMs || nowMs >= builtMs + refreshMs; }
    uint64 BuiltMs() const { return builtMs; }

    std::vector<CoaVoiceIndexedBot> const& Bots() const { return bots; }

    // Indices into Bots(). Empty when nothing is there.
    std::vector<uint32> const& InZone(uint32 mapId, uint32 zoneId) const;
    std::vector<uint32> const& InGuild(uint32 guildId) const;

    // Null when the bot was not online at the last rebuild.
    CoaVoiceIndexedBot const* Find(ObjectGuid guid) const;

    void Clear();

private:
    static uint64 ZoneKey(uint32 mapId, uint32 zoneId) { return (uint64(mapId) << 32) | zoneId; }
    void Add(Player* bot, bool fromRandomHolder);

    uint64 builtMs = 0;
    std::vector<CoaVoiceIndexedBot> bots;
    std::unordered_map<uint64, std::vector<uint32>> byZone;
    std::unordered_map<uint32, std::vector<uint32>> byGuild;
    std::unordered_map<ObjectGuid, uint32> byGuid;
    std::unordered_map<ObjectGuid, std::pair<std::string, std::string>> nameCache;  // guid -> (name, nameKey)
};

#endif
