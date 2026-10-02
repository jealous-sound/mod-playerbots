/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DUNGEONLFG_H
#define PLAYERBOTS_DUNGEONLFG_H

#include "ActivityTypes.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <optional>
#include <string>
#include <vector>

class Player;

// A 5-man dungeon a run can go to (M5), from the Dungeon Finder's data (LFGDungeons.dbc through sLFGDungeonStore and
// LFGMgr::GetLFGDungeon, entrances from lfg_dungeon_template or the map's entrance area trigger).
struct DungeonInfo
{
    uint32 id = 0;             // LFGDungeons.dbc ID (the Dungeon Finder's dungeon id)
    uint32 mapId = 0;          // the instance map
    uint8 minLevel = 0;        // the Dungeon Finder's level range: every member must be within it
    uint8 maxLevel = 0;
    uint8 targetLevel = 0;     // the level it is made for (TargetLevel), for the choice
    std::string name;          // the realm's DBC locale name, "" when none (use englishName)
    std::string englishName;   // Name[LOCALE_enUS]: logs, the `target` column, the brain's `where`
    ActivityPoint entrance;    // inside, at the entrance (LFGDungeonData x, y, z on mapId)
    ActivityPoint portal;      // outside, next to the portal: ObjectMgr::GetGoBackTrigger(mapId) target; unset when
                               // unknown (then no walking fallback: the run needs the Dungeon Finder)
};

// Where a group stands at the Dungeon Finder (lfg::LfgState without the LFGMgr header).
enum class DungeonQueueState : uint8
{
    None = 0,     // LFG_STATE_NONE: not queued (never, or a role check / proposal failed, or left)
    RoleCheck,    // LFG_STATE_ROLECHECK
    Queued,       // LFG_STATE_QUEUED
    Proposal,     // LFG_STATE_PROPOSAL
    Boot,         // LFG_STATE_BOOT
    Dungeon,      // LFG_STATE_DUNGEON: the proposal passed, everyone was sent in
    Finished,     // LFG_STATE_FINISHED_DUNGEON: the last boss died
    Other,        // LFG_STATE_RAIDBROWSER or anything else
};

char const* DungeonQueueStateName(DungeonQueueState state);  // "none", "role-check", "queued", ...

// The Dungeon Finder side of the dungeon runs. World thread only, inside the director's tick (the map threads are
// idle): JoinLfg and LeaveLfg are not thread-safe, which is why LfgJoinAction queues a packet on a map thread; here
// the calls are direct. Owned by the dungeon kind (DungeonRun.cpp's implementer).
namespace DungeonLfg
{
// The Dungeon Finder is on (DungeonFinder.OptionsMask has LFG_OPTION_ENABLE_DUNGEON_FINDER).
bool FinderEnabled();

// Every classic normal 5-man the Dungeon Finder knows with an entrance: LFG_TYPE_DUNGEON, ExpansionLevel 0,
// Difficulty normal, not seasonal, GetLFGDungeon non-null with its x, y, z set. `allowed` non-empty keeps only those
// ids. Built once and cached (the DBC and the LFG data never change while the server runs).
std::vector<DungeonInfo> const& All();
std::optional<DungeonInfo> Find(uint32 id);

// For a group of levels [minLevel, maxLevel] (the bots and the player): the dungeons whose range holds the whole
// window, of `allowed` when not empty; the one whose target level is nearest `level` (ties: urand). nullopt: none.
std::optional<DungeonInfo> Pick(std::vector<uint32> const& allowed, uint8 minLevel, uint8 maxLevel, uint8 level);

// The group's state (sLFGMgr->GetState(groupGuid)).
DungeonQueueState State(ObjectGuid groupGuid);

// `leader` (a bot session leading `groupGuid`, a full group of five not queued: State None) queues the whole group
// for this dungeon as HandleLfgJoinOpcode would: the finder on, sLFGMgr->JoinLfg(leader, role, {id}, "") with the
// leader's role (GetCoaRole for CoA classes, as LfgJoinAction::GetRoles). The other members get the role check (bots
// answer through their "lfg role check" packet action, the real player in the client). True when the group is then
// in the role check or queued; false with `why` (English) otherwise. Only a full group keeps its guid through the
// finder's proposal (LFGMgr::MakeNewGroup would replace a smaller one with a new group): fewer members, false.
bool Queue(ObjectGuid leader, ObjectGuid groupGuid, uint32 dungeonId, std::string& why);

// Takes the group out of the finder (sLFGMgr->LeaveLfg(groupGuid)) when it is in the role check, queued or at the
// proposal; nothing otherwise.
void Leave(ObjectGuid groupGuid);

// Makes a chosen bot fit its role before the group forms, as the "lfg bot" recruits are made: when its CoA role is
// not `role` (a CoaRole), PrepareCoaRecruit(anchor, bot, role, false, levelTolerance) (a specialization of the role,
// its gear, talents; kept afterwards, recorded as coa_manual_spec). Always: CoaLeaveBattlegroundQueues(bot) and
// CoaDropForeignInstanceBinds(bot, anchor) (a bind to another instance of the dungeon would keep it out). False with
// `why` when the specialization could not be switched.
bool PrepareRole(Player* bot, Player* anchor, uint8 role, uint32 levelTolerance, std::string& why);

// The bot's role for the dungeon (GetCoaRole), and whether its active specialization already fits it.
uint8 RoleOf(Player* bot);
}  // namespace DungeonLfg

#endif
