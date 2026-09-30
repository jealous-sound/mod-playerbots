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
    ObjectGuid sender;        // who spoke or emoted
    ObjectGuid target;        // the whisper's receiver, the text emote's target
    ObjectGuid groupGuid;     // party and raid chat: the group it was said in
    std::string channelName;  // CHAT_MSG_CHANNEL: the channel's name as the core holds it
    std::string text;         // the line as sent (the core caps player chat at 255 bytes)
    uint64 postedMs = 0;      // CoaTime::NowMs() when posted
};

// Bounded multi-producer, single-consumer queue from the hooks to the world thread. Map-thread hooks (text
// emotes, bot speech) and world-thread hooks (players' chat) post; CoaWorldScript::OnUpdate drains it after
// the map threads are joined. Posting takes one short lock; a disabled or closed bus returns before it.
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
