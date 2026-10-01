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

// M5: a named elite hunts go for (CoaActivity.Hunt.Targets "name:spawn:team;..."). The entry, map, position and
// level come from the spawn (creature.guid) at use; a spawn the world does not know is skipped with one LOG_WARN.
struct ActivityHuntTarget
{
    std::string name;     // [a-z0-9_]{1,24}: the `target` column, the start command's word, the brain option
    uint32 spawnId = 0;   // creature.guid
    uint8 team = 0;       // TeamId whose bots hunt it
};

// M5: an outpost raid target (CoaActivity.Raid.Targets "name:spawn:team[:rallyX:rallyY];..."): the attacking team, and
// where it rallies before the march (z from the ground). Without a rally the raid rallies RAID_RALLY_FALLBACK_YARDS
// from the target on the ground, in a random direction.
struct ActivityRaidTarget
{
    std::string name;     // [a-z0-9_]{1,24}
    uint32 spawnId = 0;   // creature.guid of the quest giver
    uint8 team = 0;       // the attacking TeamId
    bool rallySet = false;
    float rallyX = 0.0f;
    float rallyY = 0.0f;
};

// M5: a place for an inn party or a city crowd (CoaActivity.Party.Spots, Loiter.Spots: "name:team:map:x:y:z;...",
// team alliance, horde or any). Checked like a duel spot before its first use (ground height).
struct ActivitySpot
{
    std::string name;     // [a-z0-9_]{1,24}
    uint8 team = 0;       // TeamId; TEAM_NEUTRAL (2) for any
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
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

    // M5 chat recruitment.
    std::vector<std::string> chatChannels{"zone"};       // Chat.Channels: lower-case channel name prefixes where
                                                         // recruitment posts go (Chat.Channel = zone), bot on them
    uint32 chatRecruitEverySeconds = 180;                // Chat.RecruitEverySeconds: between two recruitment posts
    std::vector<std::string> chatJoinWords{"inv", "x", "moi"};  // Chat.JoinWords: whole-message whisper words a
                                                         // recruiting leader takes as a join request (lower case)
    std::string whisperPrefix = "!";                     // WhisperPrefix: "!join", "!rejoindre" whispered to any bot

    // M5 dungeon runs.
    bool dungeonEnable = true;
    uint32 weightDungeon = 30;
    uint32 dungeonBots = 4;              // [3, 4] bots; the real player (or players) fill the rest of the five
    uint32 dungeonLevelSpread = 3;       // [0, 5] bots within the player's level +- this
    uint32 dungeonMax = 1;               // [0, 4]
    uint32 dungeonRecruitMinutes = 10;   // [2, 60] the LFM stays up this long without a taker
    uint32 dungeonQueueMinutes = 10;     // [2, 60] role check, queue and proposal must be done within this
    uint32 dungeonLfgTries = 2;          // [1, 5] Dungeon Finder attempts before walking to the portal
    bool dungeonUseLfg = true;           // 0: always walk to the portal
    uint32 dungeonMaxMinutes = 120;      // [30, 360] a run lasts at most this (+ HARD_DEADLINE_EXTRA_MIN)
    uint32 dungeonIdleDisbandMinutes = 5;  // [1, 60] the bots leave this long after the player left or the run ended
    uint32 dungeonPlayerCooldownMinutes = 60;  // [0, 1440] a player is not planned for again before this
    bool dungeonSwitchSpecs = true;      // a bot of the right class but another role takes a specialization of the
                                         // role (PrepareCoaRecruit, as an "lfg bot" recruit does; it keeps it)
    std::vector<uint32> dungeonIds;      // Dungeon.Ids: LFG dungeon ids allowed; empty: every classic normal 5-man

    // M5 elite hunts.
    bool huntEnable = true;
    uint32 weightHunt = 15;
    std::vector<ActivityHuntTarget> huntTargets;
    uint32 huntMinBots = 3;              // [2, 10]
    uint32 huntMaxBots = 6;              // [MinBots, 10]
    uint32 huntMax = 1;                  // [0, 4]
    uint32 huntRecruitSeconds = 120;     // [0, 1800] the leader recruits players this long before the gathering
    uint32 huntMinutes = 20;             // [5, 120] recruiting to the kill at most (+ HARD_DEADLINE_EXTRA_MIN)
    uint32 huntLevelBelow = 2;           // [0, 10] bots from the target's level - this
    uint32 huntLevelAbove = 3;           // [0, 10] to the target's level + this

    // M5 outpost raids.
    bool raidEnable = false;
    uint32 weightRaid = 5;
    std::vector<ActivityRaidTarget> raidTargets;
    uint32 raidMinLevel = 45;            // [1, 80] bots of this level and above (the census counts them)
    uint32 raidMinBots = 6;              // [2, 40]
    uint32 raidMaxBots = 15;             // [MinBots, 39]
    uint32 raidMax = 1;                  // [0, 2]
    uint32 raidRecruitMinutes = 10;      // [1, 60]
    uint32 raidTimeoutMinutes = 25;      // [5, 180] after the recruitment (+ HARD_DEADLINE_EXTRA_MIN)
    uint32 raidQuestGiverGuardYards = 100;  // [0, 500] no engagement while a real defender is this close
    uint32 raidMaxPerTargetPerDay = 2;   // [0, 24] raids started per target and UTC day (0: no cap)
    bool raidSkipIfDefenderInZone = true;
    uint8 raidLootMethod = 3;            // Raid.LootMethod: free 0, round-robin 1, master 2, group 3, need 4
    uint8 raidLootThreshold = 2;         // [0, 6] ItemQualities

    // M5 inn parties.
    bool partyEnable = true;
    uint32 weightParty = 10;
    uint32 partyMinBots = 4;             // [2, 12]
    uint32 partyMaxBots = 8;             // [MinBots, 12]
    uint32 partyMax = 1;                 // [0, 4]
    uint32 partyMinutes = 15;            // [5, 120], +- 5 per party
    uint32 partyLevelSpread = 5;         // [0, 20] bots within the player's level +- this
    uint32 partyDancePercent = 30;       // [0, 100] dance parties among parties
    uint32 partyMaxYards = 2000;         // [100, 10000] the inn is this close to the player it is placed for
    std::vector<ActivitySpot> partySpots;  // empty: every friendly innkeeper of the world

    // M5 city loitering.
    bool loiterEnable = true;
    uint32 weightLoiter = 10;
    uint32 loiterMinBots = 3;            // [2, LOITER_MAX_BOTS]
    uint32 loiterMaxBots = 8;            // [MinBots, LOITER_MAX_BOTS]
    uint32 loiterMax = 2;                // [0, 6] crowds at a time, never two in one city
    uint32 loiterMinutes = 30;           // [5, 240], +- 10 per crowd
    uint32 loiterMinLevel = 20;          // [1, 80]
    std::vector<ActivitySpot> loiterSpots;  // empty after parsing: DefaultLoiterSpots()

    // M5 Gurubashi booty run.
    bool gurubashiEnable = true;
    uint32 weightGurubashi = 20;
    uint32 gurubashiMinBots = 4;         // [2, 8]
    uint32 gurubashiMaxBots = 8;         // [MinBots, 8]
    uint32 gurubashiMinLevel = 35;       // [1, 80]
    uint32 gurubashiMinutes = 25;        // [5, 120]
    bool gurubashiBotsMayWin = false;    // 0: participants never loot or open anything (the chest is the players')

    // M7 player-requested escorts. Weight is always zero; there are no implicit destinations.
    bool escortEnable = false;
    uint32 escortBots = 2;              // [1, 4], leaving room for the requesting player in an ordinary party
    uint32 escortMax = 2;               // [0, 4]
    uint32 escortMinutes = 15;          // [1, 30]
    uint32 escortInviteSeconds = 60;    // [10, 120]
    uint32 escortPlayerCooldownMinutes = 60;  // [0, 1440], starts only when an activity actually begins
    uint32 escortLevelSpread = 5;       // [0, 10]
    uint32 escortNearbyYards = 200;     // [20, 1000], candidates already near the requesting player
    uint32 escortArrivalYards = 20;     // [5, 100]
    std::vector<ActivitySpot> escortSpots;  // name:team:map:x:y:z; empty means no destinations

    // M7 rare scouts: solo observers, never acquire an objective or loot a rare.
    bool rareScoutEnable = false;
    uint32 weightRareScout = 0;         // [0, 1000], zero keeps ambient scheduling off
    uint32 rareScoutMax = 1;            // [0, 4]
    uint32 rareScoutMinutes = 15;       // [1, 60]
    uint32 rareScoutLevelSpread = 5;    // [0, 10]
    uint32 rareScoutNearbyYards = 500;  // [20, 2000]
    std::vector<ActivityHuntTarget> rareScoutTargets;  // name:spawn:team; no implicit rare spawns

    // AiPlayerbot.CoaBrain.Use.Duel, Use.Organizer, Duel.DeadlineMs, Duel.AcceptThreshold (read here, not by
    // BrainConfig: the brain only transports).
    bool useBrainDuel = false;
    bool useBrainOrganizer = false;
    uint32 brainDuelDeadlineMs = 3000;
    float brainDuelAcceptThreshold = 0.5f;

    bool KindEnabled(ActivityKindId kind) const;
    uint32 KindWeight(ActivityKindId kind) const;
    uint32 KindMax(ActivityKindId kind) const;  // M5: Gurubashi 1
    // M5: the bots a kind needs at least (duelMinBots, gangMinBots, dungeonBots, huntMinBots, raidMinBots,
    // partyMinBots, loiterMinBots, gurubashiMinBots); 0 for None.
    uint32 KindMinBots(ActivityKindId kind) const;
    ActivityDuelSpot const* FindSpot(std::string const& name) const;
    // M5, case-insensitive; null when absent.
    ActivityHuntTarget const* FindHuntTarget(std::string const& name) const;
    ActivityRaidTarget const* FindRaidTarget(std::string const& name) const;
    ActivitySpot const* FindPartySpot(std::string const& name) const;
    ActivitySpot const* FindLoiterSpot(std::string const& name) const;
    ActivitySpot const* FindEscortSpot(std::string const& name) const;
    ActivityHuntTarget const* FindRareScoutTarget(std::string const& name) const;
    // Pure offline checks of parser rejection, legacy enums and request ownership/throttling; no config reads.
    static std::vector<std::string> SelfTest();

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
    // M5 defaults (the conf ships the same strings): Hogger and Grand Foreman Puzik Gallywix; Magistrate Henry Maleb
    // and High Executor Darthalia with their rallies; the six classic capitals' banks (approximate, checked before
    // use).
    static char const* DefaultHuntTargets();
    static char const* DefaultRaidTargets();
    static char const* DefaultLoiterSpots();
};

#endif
