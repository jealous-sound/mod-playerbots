/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYCONFIG_H
#define PLAYERBOTS_ACTIVITYCONFIG_H

#include "ActivityTypes.h"
#include "Define.h"
#include <memory>
#include <string>
#include <vector>

// A duel spot: where a duel ring gathers. From CoaActivity.Duel.Spots, or the compiled-in defaults when the key is
// empty. The director checks each spot once, on the world thread, before its first use (ground height, area with
// AREA_FLAG_ALLOW_DUELS); a spot that fails is skipped with one LOG_WARN.
struct ActivityDuelSpot
{
    std::string name;   // [a-z0-9_]{1,24}, the `target` column and the start command's word
    uint8 team = 0;     // TeamId: TEAM_ALLIANCE 0, TEAM_HORDE 1
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint8 minLevel = 1;
    uint8 maxLevel = 60;
};

// AiPlayerbot.CoaActivity.* (plus the four AiPlayerbot.CoaBrain keys the activity reads) as one immutable snapshot.
// Load runs on every config load (startup, .reload config, .playerbots activity reload) and CoaWorldScript publishes
// it. With Enable = 0 only that key is read. Load runs before PlayerbotAIConfig::Initialize at startup, and before
// the DBC and maps: it reads sConfigMgr only (spot checks wait for the director).
struct ActivityConfig
{
    bool enable = false;
    uint32 tickMs = 1000;
    uint32 scheduleSeconds = 600;
    uint32 maxConcurrent = 2;
    uint32 maxParticipants = 60;
    uint32 maxParticipantsPercent = 20;  // of the online random bots
    uint32 suspendAboveMs = 120;
    uint32 abortAboveMs = 200;
    uint32 maxOpsPerTick = 20;
    uint32 botCooldownMinutes = 90;
    uint32 acceptPercent = 70;           // join dice without a brain
    bool requireRealPlayerOnline = true;
    uint32 ambientNearPlayerBrackets = 1;
    uint32 reviveAfterSeconds = 45;
    uint32 brainDeadlineMs = 1500;       // Organizer questions: the dice decide after this
    uint32 logKeepDays = 30;

    // Duel rings.
    bool duelEnable = true;
    uint32 weightDuel = 30;
    std::vector<ActivityDuelSpot> duelSpots;
    uint32 duelMinBots = 4;
    uint32 duelMaxBots = 8;
    uint32 duelMax = 1;
    uint32 duelMinutes = 30;             // a ring lasts duelMinutes +- 10 (at least 5)
    uint32 duelCallBoardMinHp = 60;
    uint32 duelPairCooldownMinutes = 10;
    uint32 duelPlayerCooldownMinutes = 10;
    uint32 duelMaxAcceptsPerPlayerPerDay = 3;  // 0: no cap
    bool duelPreferCallBoardHolders = false;
    bool duelAmbient = false;

    // Gang parties.
    bool gangEnable = true;
    uint32 weightGang = 20;
    uint32 gangMinBots = 3;
    uint32 gangMaxBots = 5;
    uint32 gangMax = 2;
    uint32 gangMinutes = 45;             // a gang lasts gangMinutes +- 15 (at least 10)
    uint32 gangLevelSpread = 3;          // members within the leader's level +- this

    bool pvpWarMode = false;             // PvpRuleset: "keep" (false) or "warmode" (gangs only)

    uint32 travelWalkMaxYards = 700;
    uint32 travelStragglerYards = 150;
    uint32 travelStragglerSeconds = 45;
    uint32 travelRallyWaitSeconds = 300;
    uint32 travelGatherTimeoutSeconds = 600;

    ActivityChatChannel chatChannel = ActivityChatChannel::Zone;
    uint32 chatMaxPosts = 4;             // Zone/World posts per activity
    uint32 chatMinGapSeconds = 60;

    // AiPlayerbot.CoaBrain.Use.Duel, Use.Organizer, Duel.DeadlineMs, Duel.AcceptThreshold (read here, not by
    // BrainConfig: the brain only transports).
    bool useBrainDuel = false;
    bool useBrainOrganizer = false;
    uint32 brainDuelDeadlineMs = 3000;
    float brainDuelAcceptThreshold = 0.5f;

    bool KindEnabled(ActivityKindId kind) const;
    uint32 KindWeight(ActivityKindId kind) const;
    uint32 KindMax(ActivityKindId kind) const;
    ActivityDuelSpot const* FindSpot(std::string const& name) const;

    // World thread. Reads and clamps the keys (the contract lists the clamps); a bad Duel.Spots entry is skipped
    // with one LOG_ERROR, and an empty list after parsing falls back to the defaults.
    static std::shared_ptr<ActivityConfig const> Load();
    // World thread. Stores the snapshot under a function-static mutex and sets Enabled().
    static void Publish(std::shared_ptr<ActivityConfig const> config);
    // Any thread: a copy of the pointer under that mutex; a default (disabled) snapshot before the first Publish.
    static std::shared_ptr<ActivityConfig const> Current();
    // Any thread, one relaxed load: CoaActivity.Enable as published. Whether the director actually runs (tables
    // found) is ActivityDirector::Running().
    static bool Enabled();

    // The compiled-in spots used when Duel.Spots is empty: Orgrimmar and Ironforge gates (levels 10-60), Razor Hill
    // and Goldshire (levels 1-12). Approximate coordinates: the director checks them before use.
    static char const* DefaultDuelSpots();
};

#endif
