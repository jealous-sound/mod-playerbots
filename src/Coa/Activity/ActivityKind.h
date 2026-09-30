/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYKIND_H
#define PLAYERBOTS_ACTIVITYKIND_H

#include "ActivityCandidates.h"
#include "ActivityTypes.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ActivityDuelCooldowns;
class ActivityOperations;
class Player;
struct ActivityConfig;
struct ActivityDuelResult;

// Everything below is the director's world: world thread only, inside the director's tick.

// A member of a live activity. Bots are found again every tick by guid and epoch; nothing keeps a Player.
struct ActivityMember
{
    ObjectGuid guid;
    uint64 epoch = 0;         // the bot's CoaEpoch at enrollment; 0 for a real player
    bool isBot = true;
    ActivityRole role = ActivityRole::Member;
    uint8 level = 0;          // at enrollment
    uint32 joinedUnix = 0;
    uint32 leftUnix = 0;      // 0 while in the activity
    ActivityLeaveReason leftReason = ActivityLeaveReason::None;
    bool leftHandled = false; // the director processed the leave (registry, strategies, store, ruleset, group)
    bool rulesetApplied = false;  // the director ran ApplyWarMode for it (Activity::warMode)
    uint32 prevRuleset = 0;   // ApplyWarMode's record; 0 = nothing to restore

    // Director bookkeeping (the kinds read, never write).
    bool arrived = false;          // Gathering: within the center's radius once
    uint64 deadSinceMs = 0;        // first tick seen dead, 0 while alive
    uint64 stragglerSinceMs = 0;   // first tick seen too far, 0 when close
    uint64 lastTeleportMs = 0;     // last Teleport attempt (Refused ones retried every 5 s)

    // Ring bookkeeping (DuelGathering writes).
    uint32 duelsWon = 0;
    uint32 duelsLost = 0;

    bool In() const { return leftUnix == 0; }
};

// A kind's own state, carried from Plan to Begin and kept in the Activity. Kinds derive from it in their own files.
struct ActivityKindState
{
    virtual ~ActivityKindState() = default;
};

// A live activity, owned by the director (std::unique_ptr in its map, id order). Kinds change its phase, center,
// flags and members through the methods below; the director turns those changes into registry rows, strategy
// resets and store writes after the kind's Tick.
struct Activity
{
    uint32 id = 0;
    ActivityKindId kind = ActivityKindId::None;
    uint8 team = 0;                   // TeamId
    bool gm = false;                  // started by .playerbots activity start: RequireRealPlayerOnline never ends it
    std::string name;                 // shown in list and lines (the gang's name; the spot's name for rings)
    std::string target;               // the `target` column: spot name, or the route's zone ids "12,40,44"
    uint8 minLevel = 1;
    uint8 maxLevel = 60;

    ActivityPhase phase = ActivityPhase::Planning;
    uint64 phaseSinceMs = 0;
    uint64 startedMs = 0;
    uint32 startedUnix = 0;
    uint64 hardDeadlineMs = 0;        // Timeout after this (kind minutes + HARD_DEADLINE_EXTRA_MIN)

    ObjectGuid leader;                // group kinds; empty for solo kinds
    ObjectGuid groupGuid;             // once FormGroup made it
    std::vector<ActivityMember> members;  // bots and real players, leader first for group kinds; left ones stay

    ActivityPoint center;             // see ActivityMemberView::center
    float radius = 0.0f;
    ActivityTravel travel = ActivityTravel::None;
    uint8 viewFlags = 0;              // ActivityViewFlag bits copied into every member's view
    ObjectGuid objective;             // M5
    uint64 ringDuelEndMs = 0;
    bool warMode = false;             // PvpRuleset = warmode for this activity: the director gives its bots War Mode
                                      // (ApplyWarMode) once they are members, and restores it when they leave

    // Director bookkeeping.
    uint64 leaderBestMs = 0;          // Marching: when the leader last got LEADER_PROGRESS_YARDS closer
    float leaderBestDist = 0.0f;
    uint64 holdSinceMs = 0;           // the current regroup hold began (0: none)
    uint64 holdEndedMs = 0;
    uint32 posts = 0;                 // Zone/World posts made (Chat.MaxPosts)
    uint64 lastPostMs = 0;
    std::map<ObjectGuid, uint64> invites;  // real players invited (.playerbots activity join) -> expiry
    ActivityPhase storedPhase = ActivityPhase::Planning;  // what the store has

    // The end, set by Finish; the director ends the activity after the kind's Tick.
    bool finishing = false;
    ActivityOutcome outcome = ActivityOutcome::None;
    std::string summary;              // <= 255 bytes: the `summary` column

    // Set by the methods below, cleared by the director once handled.
    bool viewDirty = false;           // a field the views copy changed: republish
    bool membershipDirty = false;     // a member joined or left: republish and reset strategies

    std::unique_ptr<ActivityKindState> state;

    ActivityMember* FindMember(ObjectGuid guid);
    ActivityMember const* FindMember(ObjectGuid guid) const;
    ActivityMember* Leader() { return leader.IsEmpty() ? nullptr : FindMember(leader); }
    uint32 BotsIn() const;
    uint32 PlayersIn() const;

    void SetPhase(ActivityPhase newPhase, uint64 nowMs);
    void SetCenter(ActivityPoint const& point, float newRadius);
    void SetTravel(ActivityTravel mode);
    void SetFlags(uint8 flags);
    // A new member (the kind at Begin, the director for joined players). Leader role for `guid == leader`.
    ActivityMember& Enroll(ObjectGuid guid, uint64 epoch, bool isBot, uint8 level, uint32 nowUnix);
    // The member leaves now (idempotent).
    void MarkLeft(ObjectGuid guid, ActivityLeaveReason reason, uint32 nowUnix);
    // Ask the director to end it after this tick (the first call wins).
    void Finish(ActivityOutcome endOutcome, std::string endSummary = {});
    // Zone/World posting limits: fewer than Chat.MaxPosts made and Chat.MinGapSeconds since the last.
    bool CanPost(ActivityConfig const& config, uint64 nowMs) const;
    void NotePost(uint64 nowMs);
};

// What a start asks for. The scheduler fills `nearPlayer` with the real player the activity is placed for; a GM start
// fills what the command said.
struct ActivityStartRequest
{
    ActivityKindId kind = ActivityKindId::None;
    bool gm = false;                  // .playerbots activity start: no roll, no real-player or bracket rule
    std::optional<uint8> team;        // TeamId forced
    uint8 level = 0;                  // forced level (ring level, gang leader level); 0: choose
    std::string target;               // duel: spot name; "" chooses
    uint32 size = 0;                  // forced bot count, clamped to the kind's MinBots..MaxBots; 0: MaxBots
    Player* nearPlayer = nullptr;     // placed for this real player (or the GM's own character); may be null
    uint32 capacity = 0;              // participant places left (MaxParticipants, MaxParticipantsPercent)
};

// A plan: what Plan found, what the join decisions keep, what Begin starts.
struct ActivityPlan
{
    ActivityKindId kind = ActivityKindId::None;
    bool gm = false;
    uint8 team = 0;
    uint8 minLevel = 1;
    uint8 maxLevel = 60;
    ActivityPoint rally;
    std::string target;
    std::string name;
    uint32 wanted = 0;                        // bots to enroll at most (leader included)
    uint32 needed = 0;                        // bots needed at least (leader included)
    bool leaderFixed = false;                 // group kinds: candidates[0] leads and is never asked
    std::vector<ActivityCandidate> candidates;  // shortlist, best first, at most 2 x wanted (reserved while planning)
    std::vector<int8> decisions;              // same order: 1 join, 0 no, -1 undecided (the director fills it)
    std::unique_ptr<ActivityKindState> state; // the kind's data for Begin (spot, route)
};

// What a kind may use during a call. References valid for that call only.
struct ActivityServices
{
    ActivityConfig const& config;
    ActivityOperations& ops;
    ActivityCandidates& candidates;
    ActivityDuelCooldowns& duels;
    std::vector<Player*> const& realPlayers;  // online real players (non-bot sessions in world), this tick
    uint64 nowMs;                             // CoaTime::NowMs()
    uint32 nowUnix;                           // time(nullptr)
};

// A kind of activity. One instance per kind lives in the director (created by CreateDuelGatheringKind and
// CreateGangPartyKind); it keeps no state of its own between calls: everything lives in the Activity. World thread,
// inside the director's tick.
class ActivityKind
{
public:
    virtual ~ActivityKind() = default;

    virtual ActivityKindId Id() const = 0;

    // Builds a plan for `request`: team, level band, rally, target, name, wanted/needed, and the candidate shortlist
    // (ActivityCandidates::Find, at most 2 x wanted). False with `why` (English, for the GM) when nothing fits now.
    virtual bool Plan(ActivityServices& services, ActivityStartRequest const& request, ActivityPlan& plan,
                      std::string& why) = 0;

    // The join decisions are in (plan.decisions). Fills `activity` (members enrolled in order, leader, center,
    // travel, flags, deadlines, phase, name, target, state moved from the plan) from the accepted candidates. The
    // director has set id, kind, team, gm, startedMs/startedUnix. False: not enough accepted (the director drops it).
    virtual bool Begin(ActivityServices& services, ActivityPlan& plan, Activity& activity) = 0;

    // Every director tick while it lives, after the director's pass (liveness, revive, travel, stragglers,
    // invitations). Moves phases, calls operations, says lines, calls Finish.
    virtual void Tick(ActivityServices& services, Activity& activity) = 0;

    // A fought duel where the winner or the loser is a member (duel rings).
    virtual void OnDuelResult(ActivityServices& /*services*/, Activity& /*activity*/,
                              ActivityDuelResult const& /*result*/)
    {
    }

    // A member left (handled by the director just before). The kind may Finish.
    virtual void OnMemberLeft(ActivityServices& /*services*/, Activity& /*activity*/, ActivityMember const& /*member*/)
    {
    }

    // .playerbots activity join by a real player who passed the director's checks (team, level band +-5, not a
    // member, in world). Returns the line to show (a coa_act_join_* text, filled). Group kinds invite (InvitePlayer)
    // and record `activity.invites[player] = now + INVITE_TIMEOUT_MS`; solo kinds describe where to go.
    virtual std::string Join(ActivityServices& services, Activity& activity, Player* player) = 0;

    // The director is ending it (any outcome), after the registry dropped its rows and before the group is
    // disbanded: last lines for a Success are said in Resolving, this is for cleanup of the kind's own state.
    virtual void OnEnd(ActivityServices& /*services*/, Activity& /*activity*/) {}

    // One line for .playerbots activity info (after the director's generic lines).
    virtual std::string Describe(Activity const& activity, uint64 nowMs) const = 0;
};

// Helpers for the director and the kinds (ActivityKind.cpp). World thread.
namespace ActivityWorld
{
// The member's Player when it is in the world: bots also need a PlayerbotAI of the member's epoch.
Player* FindMember(ActivityMember const& member);
// ActivityPoint of a player's position (zone included).
ActivityPoint PointOf(Player* player);
// A point around `center` at 0.3..1.0 x `spread` yards, on the ground (Map::GetHeight on the base map, not in water),
// zone set; 4 tries, then `center` itself with its z snapped to the ground (unset when even that has no ground).
ActivityPoint GroundNear(ActivityPoint const& center, float spread);
// Any of `realPlayers` within `range` yards of the point (same map; instance ignored for the overworld).
bool RealPlayerNear(std::vector<Player*> const& realPlayers, ActivityPoint const& point, float range);
// PlayerbotTextMgr::GetBotTextOrDefault(name, english, placeholders).
std::string Line(char const* name, char const* english, std::map<std::string, std::string> const& placeholders = {});
// The localized name of a zone or area (PlayerbotAI::GetLocalizedAreaName), "" when unknown.
std::string AreaName(uint32 areaOrZoneId);
// The first word of a character's name (as the voice director's %player).
std::string FirstName(Player* player);
}  // namespace ActivityWorld

#endif
