/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYTYPES_H
#define PLAYERBOTS_ACTIVITYTYPES_H

#include "CoaHash.h"
#include "Define.h"
#include <algorithm>
#include <optional>
#include <string_view>

// Plain values shared by the director (world thread), the kinds, the store and the bots' strategies (map threads).
// Nothing here points at a game object.

// What an activity is. The number is stored nowhere (the database keeps ActivityKindName); append only.
enum class ActivityKindId : uint8
{
    None = 0,
    Duel,       // a duel ring at a duel spot (DuelGathering): solo participants, no group
    Gang,       // a roaming PvE crew (GangParty): one bot-led group
    Dungeon,    // M5: a bot-led 5-man group recruited in chat for a real player, queued at the Dungeon Finder
    Hunt,       // M5: a bot-led group kills a named elite, open to players (EliteHunt)
    Raid,       // M5: a bot-led raid on a Hillsbrad quest giver, recruited in chat (OutpostRaid)
    Party,      // M5: bots hang out at an inn: drinks, dances, greetings (InnParty); solo participants
    Loiter,     // M5: bots loiter in a capital and show their mounts (CityLoiter); solo participants
    Gurubashi,  // M5: bots gather at the Gurubashi Arena for the booty run (BootyRun); solo participants
    Count
};

// "duel", "gang", "dungeon", "hunt", "raid", "party", "loiter", "gurubashi"; "none" otherwise. The `kind` column of
// playerbots_coa_activity and the command words.
char const* ActivityKindName(ActivityKindId kind);
// Case-insensitive; nullopt for anything else ("none" included).
std::optional<ActivityKindId> ActivityKindFromName(std::string_view name);

// The kinds whose bots form one group (FormGroup): the director's group rules (member pass, leaves, disband, join)
// apply to them. The others have solo participants.
constexpr bool ActivityKindIsGroup(ActivityKindId kind)
{
    return kind == ActivityKindId::Gang || kind == ActivityKindId::Dungeon || kind == ActivityKindId::Hunt ||
           kind == ActivityKindId::Raid;
}

// Phases. The value is the `state` column of playerbots_coa_activity while the activity lives (0-7); an ended one is
// stored as ACTIVITY_STATE_DONE or ACTIVITY_STATE_ABORTED. Never renumber.
enum class ActivityPhase : uint8
{
    Planning = 0,    // candidates reserved, join decisions pending (brain or dice); never stored
    Recruiting = 1,  // M5: the group exists and its leader posts in chat and takes real players' join requests
    Forming = 2,     // group kinds: the group is being created (FormGroup)
    Gathering = 3,   // members travel to the rally point (M5 dungeon runs: the group waits at the Dungeon Finder)
    Marching = 4,    // group kinds: the leader walks to the next stop, the members follow
    Active = 5,      // at the objective: the ring duels, the gang hunts, the hunt or raid is engaged, the run is inside
    Resolving = 6,   // last lines and emotes, then the end
    Disbanding = 7,  // M5 raids: the lead went to a real player member; the bots leave the group one by one
};

constexpr uint8 ACTIVITY_STATE_DONE = 10;
constexpr uint8 ACTIVITY_STATE_ABORTED = 11;

// "planning", "recruiting", "forming", "gathering", "marching", "active", "resolving", "disbanding".
char const* ActivityPhaseName(ActivityPhase phase);

// The `role` column of playerbots_coa_activity_member.
enum class ActivityRole : uint8
{
    Member = 0,
    Leader = 1,  // group kinds: the group's leader and the one who walks the route
};

// The `outcome` column of playerbots_coa_activity. Success is the only one that is not an abort (state 10).
enum class ActivityOutcome : uint8
{
    None = 0,
    Success = 1,     // the kind ran to its end
    Timeout = 2,     // the hard deadline passed (kind minutes + 20)
    TooFew = 3,      // not enough members to start or to go on
    LeaderGone = 4,  // group kinds: the leader logged out, died for good or left the group
    LoadAbort = 5,   // the load guard (AbortAboveMs for 60 s): the largest activity is ended
    Stopped = 6,     // .playerbots activity stop
    Disabled = 7,    // CoaActivity.Enable turned off by .reload config
    Shutdown = 8,    // the server stopped (OnShutdown)
    Restart = 9,     // found live at startup: the server crashed or was killed while it ran
    NoPlayers = 10,  // RequireRealPlayerOnline: no real player online for 10 minutes (not for GM starts)
    Guarded = 11,    // M5: a guard stopped it: a real player needs the target for a quest or holds a quest from it, or
                     // a real defender stayed near the target
};

// "none", "success", "timeout", "too-few", "leader-gone", "load", "stopped", "disabled", "shutdown", "restart",
// "no-players", "guarded".
char const* ActivityOutcomeName(ActivityOutcome outcome);

// Why a member left before the end (logs, info; not stored).
enum class ActivityLeaveReason : uint8
{
    None = 0,
    Gone,      // logged out or relogged (another epoch): not found on a tick
    Dropped,   // not at the rally in time, or a teleport never allowed
    Grouped,   // joined a group that is not the activity's (a real player's invite)
    Left,      // a real player left the group
    Kicked,    // GM or kind decision
    Ended,     // the activity ended
};

char const* ActivityLeaveReasonName(ActivityLeaveReason reason);

// Where a bot says an activity line (ActivityOperations::PostToChannel).
enum class ActivityChatTarget : uint8
{
    Say = 0,  // /say around the bot
    Yell,     // /yell
    Party,    // its group (reaches real players only)
    Zone,     // the zone's General channel (AiPlayerbot.ZoneChannelId), SayToChannel(GENERAL)
    World,    // BroadcastWorldChannelName (SayToWorld)
    Whisper,  // one player
};

// CoaActivity.Chat.Channel: where recruitment and ring announcements go ("zone", "world", "none").
enum class ActivityChatChannel : uint8
{
    None = 0,
    Zone,
    World,
};

// What the director does about a member's position during a phase (the kinds set it with the phase).
enum class ActivityTravel : uint8
{
    None = 0,        // nothing
    Gather,          // members on another map or farther than Travel.WalkMaxYards from the center are teleported
                     // (visibility rule); the others walk ("coa activity travel"); arrival within `radius`
    FollowLeader,    // group kinds: members farther than Travel.StragglerYards from the leader (or on another map)
                     // for Travel.StragglerSeconds are teleported to it; the leader's progress to the center is
                     // watched while Marching
    StayNearCenter,  // solo kinds: members farther than Travel.StragglerYards from the center for StragglerSeconds
                     // are teleported back
};

// ActivityMemberView::flags.
enum ActivityViewFlag : uint8
{
    ACTIVITY_VIEW_MAY_CHALLENGE = 0x01,       // duel ring Active: challenge ring partners ("coa duel partner")
    ACTIVITY_VIEW_MAY_CHALLENGE_REAL = 0x02,  // ... and real players passing by (with their cooldowns)
    ACTIVITY_VIEW_HOLD = 0x04,                // the leader waits where it is (Gathering, or a regroup while Marching)
    ACTIVITY_VIEW_CELEBRATE = 0x08,           // Resolving: one emote per bot
    ACTIVITY_VIEW_HUNT = 0x10,                // gang Active: hunt around the center (grind; roam when idle)
    ACTIVITY_VIEW_RECRUITING = 0x20,          // M5: the group's leader recruits in chat: a real player's whole-message
                                              // join word (Chat.JoinWords) whispered to it is a join request
    ACTIVITY_VIEW_RETREAT = 0x40,             // M5: fall back to the center (the rally) now, fighting or not; the
                                              // objective is never attacked while it is set
    ACTIVITY_VIEW_QUIET = 0x80,               // M5: the bots follow their master (a real player member) as any group
                                              // bot does: every "coa activity" movement trigger stays quiet
};

// ActivityMemberView::style: how the bots of a solo M5 kind spend their Active phase ("coa party" strategy).
enum ActivityViewStyle : uint8
{
    ACTIVITY_STYLE_NONE = 0,
    ACTIVITY_STYLE_INN = 1,     // an inn hangout: sit, drink, toast, laugh, greet players who come in
    ACTIVITY_STYLE_DANCE = 2,   // a dance party: dance, cheer, greet
    ACTIVITY_STYLE_LOITER = 3,  // a city crowd: idle emotes, mounts on show
    ACTIVITY_STYLE_ARENA = 4,   // the Gurubashi Arena: roar, flex, cheer; the chest is left alone (BotsMayWin = 0)
};

// A place: map, position, and the zone found when it was chosen (0 when unknown).
struct ActivityPoint
{
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32 zoneId = 0;
    bool set = false;

    bool IsSet() const { return set; }
    // Squared 2D distance, only meaningful on the same map.
    float SqDist2d(float px, float py) const { return (x - px) * (x - px) + (y - py) * (y - py); }
};

// Fixed numbers of the activity layer (no config key; the contract lists them).
namespace ActivityThreshold
{
constexpr uint32 DUEL_SPELL = 7266;                 // Duel, cast on the target as RpgDuelAction does
constexpr uint32 CALL_BOARD_DUEL_QUEST = 80651;     // mod-hero-call-board's "win a duel" daily
constexpr float RING_IDLE_RADIUS = 12.0f;           // ring bots wander this close to the spot
constexpr float RING_LEASH_RADIUS = 20.0f;          // farther: "coa activity away" brings them back
constexpr float RING_ARRIVE_RADIUS = 25.0f;         // Gathering: arrived at the ring
constexpr float RALLY_ARRIVE_RADIUS = 15.0f;        // Gathering: arrived at a group rally point
constexpr float HUNT_RADIUS = 120.0f;               // gang Active: leash around the hunt center
constexpr float REGROUP_RADIUS = 40.0f;             // Marching: members this close to the leader count as with it
constexpr uint32 REGROUP_PERCENT = 70;              // the leader holds while fewer bots are with it
constexpr uint32 REGROUP_MAX_MS = 60 * 1000;        // one hold lasts at most this long
constexpr uint32 REGROUP_GAP_MS = 60 * 1000;        // and the next one waits this long
constexpr float CATCH_UP_YARDS = 45.0f;             // a follower farther than this walks to the leader
constexpr float TELEPORT_UNSEEN_YARDS = 150.0f;     // no real player this close to the bot or to the landing point
constexpr float LEADER_PROGRESS_YARDS = 5.0f;       // Marching: the leader must gain this much ...
constexpr uint32 DUEL_MOOD_ACCEPT = 35;             // bot challengers: accepted when DuelMood >= this (65 %)
constexpr uint32 DECLINE_COOLDOWN_MS = 30 * 60 * 1000;  // a real player who declined is left alone this long
constexpr uint32 BOT_DUEL_REST_MS = 60 * 1000;      // a bot waits this long after its own duel before challenging
constexpr uint32 CHALLENGE_TIMEOUT_MS = 30 * 1000;  // an unanswered challenge is withdrawn after this
constexpr uint32 ADD_EXTENSION_SECONDS = 600;       // ProcessBot(uint32): "add" extended by this for held bots
constexpr uint32 RESOLVE_MS = 30 * 1000;            // Resolving lasts this long
constexpr uint32 FORM_TIMEOUT_MS = 30 * 1000;       // Forming: FormGroup must succeed within this
constexpr uint32 INVITE_TIMEOUT_MS = 60 * 1000;     // a real player's invitation is waited for this long
constexpr uint32 NO_PLAYER_END_MS = 10 * 60 * 1000; // RequireRealPlayerOnline: ambient activities end after this
constexpr uint32 LOAD_ABORT_HOLD_MS = 60 * 1000;    // AbortAboveMs must hold this long, and this long between aborts
constexpr uint32 CENSUS_INTERVAL_S = 600;           // one census sample every 10 minutes
constexpr uint32 CENSUS_SAMPLES = 1008;             // 7 days of samples
constexpr uint32 RETENTION_INTERVAL_MS = 10 * 60 * 1000;
constexpr uint32 HARD_DEADLINE_EXTRA_MIN = 20;      // a kind's minutes + this: Timeout
constexpr uint32 MAX_JOIN_QUESTIONS = 20;           // join questions per brain job
constexpr uint32 LEVEL_BANDS = 6;                   // census and brackets: 1-10, 11-20, ..., 51-60 and above
// M5.
constexpr uint8 RAID_ICON_SKULL = 7;                // Group::SetTargetIcon index of the skull (RtiAction's order)
constexpr float OBJECTIVE_REACH_YARDS = 60.0f;      // "coa activity engage"/"objective": the objective this close
constexpr uint32 RAID_CENSUS_PERCENT = 150;         // the raid pool's census p10 must reach 1.5 x Raid.MinBots
constexpr uint32 CENSUS_MIN_HOUR_SAMPLES = 3;       // samples of the current hour of day needed to open the raid gate
constexpr uint32 GURUBASHI_EVENT = 16;              // game_event "Gurubashi Arena Booty Run" (180 min, lasts 120)
constexpr uint32 GURUBASHI_CHEST = 179697;          // gameobject "Arena Treasure Chest" (Short John Mithril's script)
constexpr uint32 LOITER_MAX_BOTS = 8;               // loiterers per city on a solo realm, whatever Loiter.MaxBots says
constexpr uint32 JOIN_REQUEST_GAP_MS = 5 * 1000;    // one chat join request per real player per 5 s (extra: dropped)
constexpr uint32 JOIN_REQUEST_QUEUE_MAX = 64;       // chat join requests waiting for the director at most
constexpr uint32 RECRUIT_OFFERS_PER_REQUEST = 2;    // "lfg bot ..." heard: offers whispered by recruiting leaders
constexpr float RAID_RALLY_FALLBACK_YARDS = 250.0f; // a raid target without a rally: this far from it, on the ground
// CoA rulesets (core AscensionRulesets.cpp, AscensionMercenary.cpp).
constexpr uint32 SPELL_HIGH_RISK = 1004019;
constexpr uint32 SPELL_WAR_MODE = 1004119;
constexpr uint32 SPELL_PVE = 9931032;
constexpr uint32 SPELL_MERCENARY = 9930874;
// prev_ruleset value for "no ruleset aura before": the restore removes War Mode and applies nothing.
constexpr uint32 RULESET_NONE = 1;
}  // namespace ActivityThreshold

// Level brackets of 10 levels: 0 for 1-10 ... 5 for 51 and above. The ambient rule compares brackets.
inline uint8 ActivityBracket(uint8 level) { return level <= 1 ? 0 : uint8(std::min<uint32>(5, (level - 1) / 10)); }

// Stable per-bot traits from the guid counter (FNV-1a), 0..99. Any thread; pure. GrouperType is not used: it rotates.
namespace ActivityTraits
{
inline uint32 Trait(uint32 guidLow, uint64 salt) { return uint32(CoaHash::U64(guidLow, CoaHash::U64(salt)) % 100); }
inline uint32 Bravery(uint32 guidLow) { return Trait(guidLow, 0xB7A5E); }
inline uint32 Sociability(uint32 guidLow) { return Trait(guidLow, 0x50C1A); }
inline uint32 PvpAppetite(uint32 guidLow) { return Trait(guidLow, 0x9F9F0); }
// The bot's mood for a duel, 0..99: its bravery, mixed with a draw that changes every 10 minutes. Precomputed in
// the sense that nothing waits for it: AcceptDuelAction reads it on the bot's map thread.
inline uint32 DuelMood(uint32 guidLow, uint64 nowUnix)
{
    uint32 const slot = Trait(guidLow, 0xD0E1ULL + nowUnix / 600);
    return (Bravery(guidLow) + 2 * slot) / 3;
}
}  // namespace ActivityTraits

#endif
