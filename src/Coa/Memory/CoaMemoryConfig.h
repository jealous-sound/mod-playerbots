/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAMEMORYCONFIG_H
#define PLAYERBOTS_COAMEMORYCONFIG_H

#include "Define.h"
#include "MemoryTypes.h"
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// The behaviours that read memory, each behind its own AiPlayerbot.CoaMemory.<name> switch (all 0 by default).
enum class MemoryBehaviour : uint8
{
    GreetByMemory = 0,  // "coa known player near" -> "coa greet known"
    MemoryReplies,      // the voice director's tone and lines (MemoryTone)
    Revenge,            // EnemyPlayerValue prefers grudges; a guild call after a kill
    RivalDuels,         // AcceptDuelAction: rivals, grudges and friends from 70 % health, disliked ones refused
    FriendInvites,      // "coa friend near" -> "coa offer group" (a whisper; the player invites)
    Count
};

constexpr std::size_t MEMORY_BEHAVIOUR_COUNT = std::size_t(MemoryBehaviour::Count);
constexpr std::size_t MEMORY_LEADER_ENTRIES_MAX = 32;

// "GreetByMemory", "MemoryReplies", "Revenge", "RivalDuels", "FriendInvites": the key suffixes.
char const* MemoryBehaviourName(MemoryBehaviour behaviour);

// AiPlayerbot.CoaMemory.* as one immutable snapshot, read by Load on every config load (startup and .reload
// config) and published by CoaWorldScript::OnAfterConfigLoad. With Enable = 0 only that key is read. Load runs
// before PlayerbotAIConfig::Initialize at startup: it reads sConfigMgr only.
struct CoaMemoryConfig
{
    bool enable = false;
    bool randomBotsOnly = true;   // memory for random and addclass bots only (not altbots)
    bool trackBotToBot = false;   // deeds between two bots (duels, kills, groups, trades) are remembered too
    uint32 maxRelationsPerBot = 64;
    uint32 maxEventsPerBot = 200;  // in the database
    uint32 maxEventsLoaded = 64;   // in RAM; never above maxEventsPerBot
    uint32 flushIntervalMs = 120000;
    uint32 flushBotsPerTick = 20;
    uint32 drainPerTick = 2000;    // memory events applied per world tick; the rest wait for the next ticks
    uint32 eventRetentionDays = 30;
    uint32 relationRetentionDays = 180;
    uint32 retentionBatch = 5000;  // rows per retention DELETE ... LIMIT
    // HalfLifeDays.<Axis> in MemoryAxis order.
    std::array<float, MEMORY_AXIS_COUNT> halfLifeDays = {30.0f, 60.0f, 60.0f, 10.0f, 21.0f,
                                                         14.0f, 7.0f,  3.0f,  90.0f, 45.0f};
    MemoryDecay decay;               // MemoryDecay::FromHalfLives(halfLifeDays)
    uint32 dailyChatDeltaCap = 15;   // points of affinity words and emotes may move per pair per UTC day
    uint8 sentiment = 1;             // 0 off (counters only), 1 heuristic, 2 brain with heuristic fallback
    bool useBrainRating = false;     // AiPlayerbot.CoaBrain.Use.Rating
    std::vector<std::string> friendlyWords;  // folded (NameMatch::Fold); phrases keep one space between words
    std::vector<std::string> hostileWords;
    uint32 stateTokenBudget = 300;   // tokens of [memory] in the brain state (bytes / 4); 0 = none
    uint8 battlegroundKills = 1;     // 0 ignore, 1 counters and a match-long grudge, 2 like world kills
    std::vector<uint32> leaderEntries;  // at most MEMORY_LEADER_ENTRIES_MAX
    bool orphanSweepOnStartup = true;
    std::array<bool, MEMORY_BEHAVIOUR_COUNT> behaviours{};

    bool BehaviourOn(MemoryBehaviour behaviour) const { return enable && behaviours[std::size_t(behaviour)]; }
    // The voice decision also asks friendliness, insult and thanks.
    bool UseBrainRating() const { return enable && sentiment == 2 && useBrainRating; }

    // World thread. Reads the keys (clamps in the contract) and builds `decay`. Enable = 1 with
    // PlayerbotsDatabase.WorkerThreads other than 1: one LOG_ERROR and a disabled snapshot.
    static std::shared_ptr<CoaMemoryConfig const> Load();
    // World thread. Stores the snapshot under a function-static mutex and sets the lock-free flags below.
    static void Publish(std::shared_ptr<CoaMemoryConfig const> config);
    // Any thread: a copy of the pointer under that mutex; a default (disabled) snapshot before the first Publish.
    static std::shared_ptr<CoaMemoryConfig const> Current();

    // Any thread, lock-free (relaxed atomics set by Publish): what hooks and map threads test before anything else.
    static bool Enabled();
    static bool TrackBotToBot();
    static uint8 BattlegroundKills();
    // A CoaMemory.LeaderEntries creature. False at once when the list is empty.
    static bool IsLeaderEntry(uint32 entry);
    // Enable and that switch.
    static bool BehaviourActive(MemoryBehaviour behaviour);
    // Enable and at least one switch: AiFactory then adds the "coa memory" strategy.
    static bool AnyBehaviour();
};

#endif
