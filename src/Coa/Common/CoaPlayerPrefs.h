/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAPLAYERPREFS_H
#define PLAYERBOTS_COAPLAYERPREFS_H

#include "Define.h"
#include <atomic>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ResultSet;

// Values of the ai_text, voice and memory columns of playerbots_coa_player_prefs.
enum CoaPrefValue : uint8
{
    COA_PREF_DEFAULT = 0,
    COA_PREF_IN = 1,
    COA_PREF_OUT = 2
};

// Bits of activity_mask: a set bit means the account opted out of that.
enum CoaActivityOptOut : uint32
{
    COA_OPTOUT_DUELS = 0x1,     // duel offers from bots
    COA_OPTOUT_HOSTILE = 0x2,   // hostile bot behaviour (grudges, revenge)
    COA_OPTOUT_WHISPERS = 0x4,  // unasked whispers from bots
    COA_OPTOUT_GAMBLING = 0x8   // gambling offers
};

// What .playerbots optout|optin takes.
enum class CoaPrefTopic : uint8
{
    Ai,       // player text sent to a model (M2)
    Voice,    // bots answer this player at all
    Memory,   // bots remember this player (M3)
    Duels,    // COA_OPTOUT_DUELS
    Hostile,  // COA_OPTOUT_HOSTILE
    All
};

struct CoaPrefs
{
    uint8 aiText = COA_PREF_DEFAULT;
    uint8 voice = COA_PREF_DEFAULT;
    uint8 memory = COA_PREF_DEFAULT;
    uint32 activityMask = 0;
    uint32 disclosedAt = 0;  // unix time of the last disclosure message (M2)
    uint32 changedAt = 0;    // unix time of the last change

    bool VoiceOptedOut() const { return voice == COA_PREF_OUT; }
    bool AiOptedOut() const { return aiText == COA_PREF_OUT; }
    bool MemoryOptedOut() const { return memory == COA_PREF_OUT; }
    bool IsDefault() const { return !aiText && !voice && !memory && !activityMask; }
};

// Per-account preferences, cached whole in memory. Loaded once: synchronously at startup when the voice is on,
// otherwise by an async query (the .reload config that turns the voice on, or the first optout/optin command).
// Written with async REPLACE INTO when a player changes them. No query ever runs on a map thread; the only
// readers off the world thread take a shared lock for a hash lookup.
class CoaPlayerPrefs
{
public:
    static CoaPlayerPrefs& Instance();

    // World thread, CoaWorldScript::OnStartup only: the table check, then the SELECT, both synchronous (at a first
    // start the async worker spends minutes on the item caches, and the voice would wait behind it). No-op when
    // loaded or when an async load is in flight.
    void LoadSync();

    // World thread: a .reload config that turns the voice on, the optout/optin commands. Sends the table check,
    // then the SELECT; ProcessCallbacks later fills the cache. No-op when loaded or in flight.
    void LoadAsync();

    // World thread, every CoaWorldScript::OnUpdate: runs the load callback once it is ready.
    void ProcessCallbacks();

    // Any thread. False until the load callback ran (or failed: a missing table counts as loaded and empty).
    bool IsLoaded() const { return loaded.load(std::memory_order_acquire); }

    // Any thread. Defaults for an account without a row.
    CoaPrefs Get(uint32 accountId) const;
    bool VoiceOptedOut(uint32 accountId) const;
    // Any thread (shared lock). Every account whose memory column is COA_PREF_OUT, in no order. CoaMemoryMgr purges
    // them once the preferences are loaded after memory starts: opt-outs made while memory was off.
    std::vector<uint32> MemoryOptOuts() const;

    // World thread (chat commands). Updates the cache at once, then writes the whole row with an async
    // REPLACE INTO (integers only). optOut false opts back in. Topic All covers every topic.
    void SetOptOut(uint32 accountId, CoaPrefTopic topic, bool optOut);

    // World thread, CoaWorldScript::OnShutdown: drops a load still in flight. Writes already sent are
    // flushed by PlayerbotsDatabase.Close().
    void Shutdown();

    // "ai", "voice", "memory", "duels", "hostile", "all" (case-insensitive); nullopt for anything else.
    static std::optional<CoaPrefTopic> ParseTopic(std::string_view word);
    static char const* TopicName(CoaPrefTopic topic);

private:
    CoaPlayerPrefs() = default;
    CoaPlayerPrefs(CoaPlayerPrefs const&) = delete;
    CoaPlayerPrefs& operator=(CoaPlayerPrefs const&) = delete;

    // End of either load, world thread: the cache is complete (empty and memory-only when the table is missing).
    void OnTableMissing();
    void OnLoaded(ResultSet* result);  // null: no rows

    // A SetOptOut made before the load returned: replayed over the loaded row, so the other columns survive.
    struct PendingChange
    {
        uint32 accountId;
        CoaPrefTopic topic;
        bool optOut;
        uint32 changedAt;
    };

    // The load's QueryCallbackProcessor is a file-static of CoaPlayerPrefs.cpp: it is touched on the world
    // thread only, and keeping it there keeps the database headers out of this one.
    mutable std::shared_mutex lock;
    std::unordered_map<uint32, CoaPrefs> prefs;    // guarded by lock
    std::unordered_set<uint32> changedBeforeLoad;  // world thread: rows written once the load returned
    std::atomic<bool> loaded{false};
    // World thread only.
    std::vector<PendingChange> changesBeforeLoad;  // in order
    bool loading = false;                          // the table check or the SELECT is in flight
    bool stopped = false;                          // Shutdown ran: no load, no callback, no write
    bool tableMissing = false;  // the update was not applied: the cache lives in memory only, nothing is written
};

#endif
