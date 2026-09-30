/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ACTIVITYOPERATIONS_H
#define PLAYERBOTS_ACTIVITYOPERATIONS_H

#include "ActivityTypes.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <map>
#include <string>
#include <vector>

class Channel;
class Player;

enum class ActivityOpResult : uint8
{
    Done = 0,
    Deferred,  // no budget left this tick (MaxOpsPerTick): call again next tick
    Refused,   // a rule said no this time (a real player would see the teleport, the bot is in combat...): retry later
    Failed,    // cannot be done (no group, not a member, core refused)
    Gone,      // a player it needs is not in the world
};

char const* ActivityOpResultName(ActivityOpResult result);  // "done", "deferred", "refused", "failed", "gone"

// What changes the world for the activities: groups, invites, chat posts, raid icons, leadership, teleports, CoA
// rulesets, revivals. Every function runs on the world thread only, inside the director's tick
// (COA_ASSERT_WORLD_THREAD), while the map threads are idle: nothing here goes through PlayerbotWorldThreadProcessor
// and nothing is queued. Every function looks its players up again (ObjectAccessor::FindConnectedPlayer) and never
// keeps a pointer. Bots only: a real player is never grouped, teleported, revived or given a ruleset by these,
// except InvitePlayer (an ordinary invite the player accepts or not) and HandLeader to a player already in the group.
//
// Budget: FormGroup (1 + one per member added), AddMember, InvitePlayer, Teleport, Revive and PostToChannel spend
// MaxOpsPerTick and return Deferred when it is spent. Leave, Disband, HandLeader, SetIcon and Ruleset are counted but
// never deferred (endings must finish in their tick).
class ActivityOperations
{
public:
    // The director, at the start of each tick. `realPlayers`: this tick's online real players (the teleport rule).
    void BeginTick(uint32 maxOps, std::vector<Player*> const* realPlayers);
    bool HasBudget() const { return used < budget; }
    uint32 Used() const { return used; }

    // Creates a group led by `leader` (a bot without a group) and adds `members` (bots) in order: sets the loot
    // method and threshold (uint8 LootMethod, uint8 ItemQualities), converts to a raid before the 6th member, and
    // gives every member added AcceptInvitationAction's steps (contract 9.2). A member that has a group, is not found,
    // is not a bot or is not in the world is skipped, never removed from anything. Stops adding when the budget runs
    // out (the rest is `skipped`, to add later with AddMember). Done with `groupGuid` set when the group exists,
    // even with no member added; Failed when the leader cannot lead (group, not found, Create refused).
    ActivityOpResult FormGroup(ObjectGuid leader, std::vector<ObjectGuid> const& members, uint8 lootMethod,
                               uint8 lootThreshold, ObjectGuid& groupGuid, std::vector<ObjectGuid>& added,
                               std::vector<ObjectGuid>& skipped);

    // Adds one bot to the activity group led by `leader` (raid conversion before the 6th, member steps).
    ActivityOpResult AddMember(ObjectGuid groupGuid, ObjectGuid leader, ObjectGuid member);

    // `leader` (a bot in `groupGuid`, or alone) invites the real player through HandleGroupInviteOpcode, as a player
    // would; converts the group to a raid first when it already has 5 members. The player accepts in the UI; the
    // director polls membership. Refused when the player has a group or a pending invitation.
    ActivityOpResult InvitePlayer(ObjectGuid leader, ObjectGuid player);

    // The bot says `text` (not empty; cut to 255 bytes with CoaText::CutUtf8Bytes) there. Zone: SayToChannel(GENERAL),
    // Failed when the bot is in no such channel; World: SayToWorld; Party: SayToParty (Failed without a group);
    // Whisper: to `whisperTo` (Gone when not found). Refused while the bot is in combat (Say, Yell, Zone, World).
    ActivityOpResult PostToChannel(ObjectGuid bot, ActivityChatTarget target, std::string const& text,
                                   ObjectGuid whisperTo = ObjectGuid::Empty);

    // M5 recruitment posts (Chat.Channel = zone): the bot says `text` (cut to 255 bytes) in at most two chat channels
    // of its team's ChannelMgr whose name starts with one of `prefixes` (lower case, compared case-insensitively, as
    // CoaLfgChannels and CoaVoice.Channels). Channels named after a place (id AiPlayerbot.ZoneChannelId, General or
    // LocalDefense) are chosen by who the post is for, first match wins:
    //  - `audience`, the real player it is meant for (empty: nobody; ignored when not in the world), is really on such
    //    channels (CoaChannels::Joined) that carry the name of the zone it stands in or of its area there: the bot
    //    speaks in those, joining the ones it is not on yet (Channel::JoinChannel on an existing channel, never a new
    //    one) and staying on them until LeaveActivityChannels;
    //  - else the channels the bot is really on (CoaChannels::IsOn) that carry the name of the zone `zoneId` (AreaTable
    //    id), or, when `zoneId` is 0 or the bot is on none of them, of the zone the bot stands in (or its area there);
    //    never another place's.
    // Channels of no place (Trade, LookingForGroup, a custom one) the bot is really on follow. One budget unit. Done
    // when it spoke in one channel at least; Failed when none qualifies (or `text` is empty): nothing was said, so the
    // caller does not count the post (Activity::NotePost); Refused in combat; Gone when the bot is not in the world;
    // Deferred without budget.
    ActivityOpResult PostToChannels(ObjectGuid bot, std::vector<std::string> const& prefixes, std::string const& text,
                                    uint32 zoneId = 0, ObjectGuid audience = ObjectGuid::Empty);

    // Takes the bot off the channels PostToChannels joined it to (not the ones it was on already, nor the channel of
    // the place it stands in now). Idempotent, never budgeted, no result: the director calls it when the bot leaves
    // an activity and for every bot of an activity that ends. A bot that logged out since (another CoaEpoch) has
    // nothing left to leave: the core dropped its channels with the session.
    void LeaveActivityChannels(ObjectGuid bot);

    // Group::SetTargetIcon(icon, setter, target) on the setter's group (M5 objectives).
    ActivityOpResult SetIcon(ObjectGuid setter, uint8 icon, ObjectGuid target);

    // Group::ChangeLeader to `newLeader`, a member of `groupGuid`; SendUpdate.
    ActivityOpResult HandLeader(ObjectGuid groupGuid, ObjectGuid newLeader);

    // Teleports a bot to `dest` (TeleportTo, motion cleared, teleport auras removed; the AI is not reset). The rule:
    // a bot session, alive (unless `dead` is true, for Revive), not in combat, not being teleported, not in flight, no
    // real player within TELEPORT_UNSEEN_YARDS of the bot (same map and instance) nor of `dest` (same map): otherwise
    // Refused. Real players: Failed, always.
    ActivityOpResult Teleport(ObjectGuid bot, ActivityPoint const& dest, bool dead = false);

    // War Mode for a bot (PvpRuleset = warmode): records the ruleset it had in `prevRuleset` (SPELL_PVE, or
    // RULESET_NONE when it had none; 0 when it already was in War Mode: nothing changed), removes the four ruleset
    // auras, casts SPELL_WAR_MODE triggered, sets PLAYER_FLAGS_IN_PVP and UpdatePvP(true, true). Failed for a bot with
    // High-Risk or Mercenary (never enrolled for such kinds) and for real players.
    ActivityOpResult ApplyWarMode(ObjectGuid bot, uint32& prevRuleset);
    // Undoes ApplyWarMode: removes PLAYER_FLAGS_IN_PVP, UpdatePvP(false), removes SPELL_WAR_MODE, casts
    // `prevRuleset` when it is SPELL_PVE. Nothing for prevRuleset 0. Gone when the bot is offline (the store keeps
    // the row unrestored for its next login).
    ActivityOpResult RestoreRuleset(ObjectGuid bot, uint32 prevRuleset);

    // Brings a dead bot (corpse or ghost) back at 50 % health (ResurrectPlayer(0.5f), SpawnCorpseBones), then, when
    // `rally` is set, teleports it there under Teleport's rule; when the rule refuses, the bot stays alive where it
    // is and walks back. Done also when only the resurrection happened.
    ActivityOpResult Revive(ObjectGuid bot, ActivityPoint const* rally);

    // Removes one bot from its group (Group::RemoveMember), only when that group is `groupGuid`.
    ActivityOpResult Leave(ObjectGuid groupGuid, ObjectGuid bot);

    // Ends an activity group. Random bots only (the only bots the director enrolls): Group::Disband. With anyone
    // else, online or not (real players, self-bots, a player's own altbots or addclass bots): leadership to the first
    // real player online (when a random bot or nobody online leads), then every random bot removed, so the others
    // keep their group. Done when the group is gone already.
    ActivityOpResult Disband(ObjectGuid groupGuid);

private:
    // What PostToChannels joined a bot to: channel names (looked up again when leaving, never a pointer) and the
    // bot's CoaEpoch when it joined them.
    struct ChannelJoins
    {
        uint64 epoch = 0;
        std::vector<std::string> names;
    };

    bool Spend(uint32 amount = 1);  // false (nothing spent) when the budget is short
    // True when `bot` is on `channel`: joins it when it was not, and records that join for LeaveActivityChannels.
    bool EnterChannel(Player* bot, Channel* channel);

    uint32 budget = 0;
    uint32 used = 0;
    std::vector<Player*> const* realPlayers = nullptr;
    std::map<ObjectGuid, ChannelJoins> joined;  // bot -> channels joined for a post; world thread only
};

#endif
