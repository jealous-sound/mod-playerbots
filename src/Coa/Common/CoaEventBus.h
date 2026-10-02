/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAEVENTBUS_H
#define PLAYERBOTS_COAEVENTBUS_H

#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

enum class CoaEventKind : uint8
{
    None = 0,
    Chat,       // a chat line: say, yell, /e, whisper, party/raid, guild or channel
    TextEmote,  // a text emote (/wave) aimed at a player
    // M3 memory events, from here on: CoaMemoryMgr consumes them, the voice director never sees them. Fields per
    // kind (unlisted fields stay default):
    BotLogin,          // sender = the bot, epoch = its PlayerbotAI's CoaEpoch (constructor, world thread)
    BotLogout,         // sender = the bot, epoch (~PlayerbotAI, map or world thread)
    Duel,              // sender = winner, target = loser, value = DuelCompleteType (DUEL_WON or DUEL_FLED: the
                       // loser fled), senderLevel, targetLevel, mapId, zoneId (the winner's)
    PlayerKill,        // sender = killer (the owner of a pet or totem), target = victim, senderLevel, targetLevel,
                       // mapId, zoneId (the victim's), flags COA_EVENT_BATTLEGROUND in a battleground or arena
    BossKill,          // sender = the killing player (owner resolved), entry = creature entry, groupGuid = the
                       // killer's group (empty when none), mapId, instanceId, zoneId, flags COA_EVENT_LEADER for a
                       // CoaMemory.LeaderEntries creature
    CreatureHelp,      // sender = the player who killed the creature, target = its loot recipient, groupGuid = the
                       // loot recipient group (empty when none), entry, mapId, zoneId
    GroupJoin,         // sender = the member who joined, groupGuid (not battleground or battlefield groups)
    GroupLeave,        // sender = the member removed, target = the kicker (empty unless kicked), groupGuid,
                       // value = RemoveMethod
    Trade,             // sender = the bot, target = the trader, value = distinct items the bot received,
                       // value2 = distinct items it gave, mapId, zoneId
    CharacterDeleted,  // sender = the deleted character (hard delete, OnPlayerDeleteFromDB)
};

// CoaEvent::flags of the memory events.
enum CoaEventFlag : uint8
{
    COA_EVENT_SENDER_BOT = 0x01,    // the sender's session is a bot session (WorldSession::IsBot)
    COA_EVENT_TARGET_BOT = 0x02,    // the target's session is a bot session
    COA_EVENT_BATTLEGROUND = 0x04,  // PlayerKill in a battleground or an arena
    COA_EVENT_LEADER = 0x08         // BossKill of a CoaMemory.LeaderEntries creature
};

// What a hook saw, copied into plain values on the hook's own thread. Never a pointer to a game object: the
// consumer looks every GUID up again on the world thread.
struct CoaEvent
{
    CoaEventKind kind = CoaEventKind::None;
    uint8 chatType = 0;       // ChatMsg of a Chat event
    uint8 teamId = 0;         // TeamId of the speaker
    uint8 chainDepth = 0;     // 0 for a real player; a bot line's hop: 1, or n + 1 when it answers a hop n line
    bool fromBot = false;     // a bot said it; posted only with AiPlayerbot.CoaVoice.BotToBot = 1
    uint32 textEmote = 0;     // TextEmotes id of a TextEmote event
    uint32 guildId = 0;       // the speaker's guild, for CHAT_MSG_GUILD
    ObjectGuid sender;        // who spoke or emoted; the subject of a memory event
    ObjectGuid target;        // the whisper's receiver, the text emote's target; the other side of a memory event
    ObjectGuid groupGuid;     // party and raid chat: the group it was said in; the group of a memory event
    std::string channelName;  // CHAT_MSG_CHANNEL: the channel's name as the core holds it
    std::string text;         // the line as sent (the core caps player chat at 255 bytes)
    uint64 postedMs = 0;      // CoaTime::NowMs() when posted
    // M6: a BOT event made by a banter opener: the chance in percent that a bot answers it
    // (CoaVoiceLine::answerPercent). The director uses it instead of the scope's HeuristicChance, decayed per hop
    // after the first. 0: the scope's own.
    uint8 answerPercent = 0;

    // M3 memory events only (see CoaEventKind).
    uint64 epoch = 0;        // BotLogin, BotLogout
    uint32 entry = 0;        // BossKill, CreatureHelp: creature entry
    uint32 value = 0;        // Duel: DuelCompleteType; GroupLeave: RemoveMethod; Trade: items received
    uint32 value2 = 0;       // Trade: items given
    uint32 mapId = 0;
    uint32 instanceId = 0;   // BossKill
    uint32 zoneId = 0;
    uint8 senderLevel = 0;
    uint8 targetLevel = 0;
    uint8 flags = 0;         // CoaEventFlag bits
};

// Whether an event belongs to CoaMemoryMgr (true) or to the voice director (Chat, TextEmote).
inline bool IsCoaMemoryEvent(CoaEventKind kind) { return kind >= CoaEventKind::BotLogin; }

// Bounded multi-producer, single-consumer queue from the hooks to the world thread. Map-thread hooks (text
// emotes, bot speech, memory deeds) and world-thread hooks (players' chat) post; CoaWorldScript::OnUpdate drains
// it after the map threads are joined. Posting takes one short lock; a disabled or closed bus returns before it.
// Enabled while CoaVoice.Enable or CoaMemory.Enable is 1 (CoaWorldScript::OnAfterConfigLoad).
class CoaEventBus
{
public:
    static CoaEventBus& Instance();

    // World thread, at config load. A capacity of 0 is read as 1.
    void Configure(uint32 capacity);
    void SetEnabled(bool enable) { enabled.store(enable, std::memory_order_relaxed); }
    bool IsEnabled() const
    {
        return enabled.load(std::memory_order_relaxed) && !closed.load(std::memory_order_relaxed);
    }

    // Any thread. False when the bus is disabled, closed or full. A full bus drops this event (the newest)
    // and counts the drop; what is already queued stays.
    bool Post(CoaEvent&& event);

    // World thread. Moves every queued event to the end of `out`, oldest first.
    void Drain(std::vector<CoaEvent>& out);

    // World thread, OnShutdown. Disables the bus for good and frees what is queued: late events (the bots
    // logged out by KickAll after the last tick) are dropped without being counted.
    void Close();

    uint64 Posted() const { return posted.load(std::memory_order_relaxed); }
    uint64 Dropped() const { return dropped.load(std::memory_order_relaxed); }
    uint32 Pending() const { return pending.load(std::memory_order_relaxed); }
    uint32 Capacity() const { return capacity.load(std::memory_order_relaxed); }

private:
    CoaEventBus() = default;
    CoaEventBus(CoaEventBus const&) = delete;
    CoaEventBus& operator=(CoaEventBus const&) = delete;

    std::mutex lock;
    std::vector<CoaEvent> queue;  // guarded by lock
    std::atomic<uint32> capacity{20000};
    std::atomic<bool> enabled{false};
    std::atomic<bool> closed{false};
    std::atomic<uint64> posted{0};
    std::atomic<uint64> dropped{0};
    std::atomic<uint32> pending{0};
};

#endif
