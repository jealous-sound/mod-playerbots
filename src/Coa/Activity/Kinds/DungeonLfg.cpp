/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DungeonLfg.h"
#include "AscensionSpecialization.h"
#include "CoaSpecialization.h"
#include "CoaThread.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <cstdlib>
#include <limits>

namespace
{
constexpr char const* LOG = "playerbots.activity";

uint8 Level8(uint32 level) { return uint8(std::min<uint32>(level, 255)); }

// The zone of a point on a map the world keeps loaded (a continent); 0 elsewhere.
uint32 ZoneAt(uint32 mapId, float x, float y, float z)
{
    MapEntry const* entry = sMapStore.LookupEntry(mapId);
    if (!entry || entry->Instanceable())
        return 0;
    Map* map = sMapMgr->CreateBaseMap(mapId);
    return map ? map->GetZoneId(PHASEMASK_NORMAL, x, y, z) : 0;
}

std::vector<DungeonInfo> BuildDungeons()
{
    std::vector<DungeonInfo> dungeons;
    LocaleConstant const locale = sWorld->GetDefaultDbcLocale();
    for (uint32 row = 0; row < sLFGDungeonStore.GetNumRows(); ++row)
    {
        LFGDungeonEntry const* entry = sLFGDungeonStore.LookupEntry(row);
        if (!entry || entry->TypeID != lfg::LFG_TYPE_DUNGEON || entry->ExpansionLevel != 0 ||
            entry->Difficulty != DUNGEON_DIFFICULTY_NORMAL || (entry->Flags & lfg::LFG_FLAG_SEASONAL))
            continue;
        lfg::LFGDungeonData const* data = sLFGMgr->GetLFGDungeon(entry->ID);
        if (!data || (!data->x && !data->y && !data->z))
            continue;

        DungeonInfo info;
        info.id = entry->ID;
        info.mapId = data->map;
        info.minLevel = Level8(entry->MinLevel);
        info.maxLevel = Level8(entry->MaxLevel);
        info.targetLevel = Level8(entry->TargetLevel);
        if (entry->Name[locale] && *entry->Name[locale])
            info.name = entry->Name[locale];
        if (entry->Name[LOCALE_enUS] && *entry->Name[LOCALE_enUS])
            info.englishName = entry->Name[LOCALE_enUS];
        if (info.englishName.empty())
            info.englishName = !info.name.empty() ? info.name : "dungeon " + std::to_string(info.id);

        info.entrance.mapId = data->map;
        info.entrance.x = data->x;
        info.entrance.y = data->y;
        info.entrance.z = data->z;
        MapEntry const* mapEntry = sMapStore.LookupEntry(data->map);
        info.entrance.zoneId = mapEntry ? mapEntry->linked_zone : 0;
        info.entrance.set = true;

        // Outside, next to the portal: where the instance's exit trigger sends players, on a continent only.
        if (AreaTriggerTeleport const* back = sObjectMgr->GetGoBackTrigger(data->map))
        {
            MapEntry const* outside = sMapStore.LookupEntry(back->target_mapId);
            if (outside && !outside->Instanceable())
            {
                info.portal.mapId = back->target_mapId;
                info.portal.x = back->target_X;
                info.portal.y = back->target_Y;
                info.portal.z = back->target_Z;
                info.portal.zoneId = ZoneAt(back->target_mapId, back->target_X, back->target_Y, back->target_Z);
                info.portal.set = true;
            }
        }
        dungeons.push_back(std::move(info));
    }

    std::sort(dungeons.begin(), dungeons.end(),
              [](DungeonInfo const& a, DungeonInfo const& b) { return a.id < b.id; });
    LOG_DEBUG(LOG, "{} dungeons for dungeon runs", dungeons.size());
    return dungeons;
}

// The Dungeon Finder role of a member, as LfgJoinAction::GetRoles: CoA classes from their specialization.
uint8 RoleMask(Player* player)
{
    if (IsAscensionCustomClassId(player->getClass()))
    {
        switch (GetCoaRole(player))
        {
            case CoaRole::Tank:
                return lfg::PLAYER_ROLE_TANK;
            case CoaRole::Heal:
                return lfg::PLAYER_ROLE_HEALER;
            default:
                return lfg::PLAYER_ROLE_DAMAGE;
        }
    }
    if (PlayerbotAI::IsTank(player))
        return lfg::PLAYER_ROLE_TANK;
    if (PlayerbotAI::IsHeal(player))
        return lfg::PLAYER_ROLE_HEALER;
    return lfg::PLAYER_ROLE_DAMAGE;
}
}  // namespace

char const* DungeonQueueStateName(DungeonQueueState state)
{
    switch (state)
    {
        case DungeonQueueState::None:
            return "none";
        case DungeonQueueState::RoleCheck:
            return "role-check";
        case DungeonQueueState::Queued:
            return "queued";
        case DungeonQueueState::Proposal:
            return "proposal";
        case DungeonQueueState::Boot:
            return "boot";
        case DungeonQueueState::Dungeon:
            return "dungeon";
        case DungeonQueueState::Finished:
            return "finished";
        default:
            return "other";
    }
}

namespace DungeonLfg
{
bool FinderEnabled() { return sLFGMgr->isOptionEnabled(lfg::LFG_OPTION_ENABLE_DUNGEON_FINDER); }

std::vector<DungeonInfo> const& All()
{
    COA_ASSERT_WORLD_THREAD();
    static std::vector<DungeonInfo> const dungeons = BuildDungeons();
    return dungeons;
}

std::optional<DungeonInfo> Find(uint32 id)
{
    for (DungeonInfo const& dungeon : All())
        if (dungeon.id == id)
            return dungeon;
    return std::nullopt;
}

std::optional<DungeonInfo> Pick(std::vector<uint32> const& allowed, uint8 minLevel, uint8 maxLevel, uint8 level)
{
    std::vector<DungeonInfo const*> best;
    int32 bestGap = std::numeric_limits<int32>::max();
    for (DungeonInfo const& dungeon : All())
    {
        if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), dungeon.id) == allowed.end())
            continue;
        if (minLevel < dungeon.minLevel || maxLevel > dungeon.maxLevel)
            continue;
        int32 const gap = std::abs(int32(dungeon.targetLevel) - int32(level));
        if (gap < bestGap)
        {
            best.clear();
            bestGap = gap;
        }
        if (gap == bestGap)
            best.push_back(&dungeon);
    }
    if (best.empty())
        return std::nullopt;
    return *best[urand(0, uint32(best.size()) - 1)];
}

DungeonQueueState State(ObjectGuid guid)
{
    // LFGMgr::GetState creates a row for a guid it does not know: only ask about live groups and online players.
    if (guid.IsEmpty())
        return DungeonQueueState::None;
    if (guid.IsGroup())
    {
        if (!sGroupMgr->GetGroupByGUID(guid.GetCounter()))
            return DungeonQueueState::None;
    }
    else if (!ObjectAccessor::FindConnectedPlayer(guid))
        return DungeonQueueState::None;

    switch (sLFGMgr->GetState(guid))
    {
        case lfg::LFG_STATE_NONE:
            return DungeonQueueState::None;
        case lfg::LFG_STATE_ROLECHECK:
            return DungeonQueueState::RoleCheck;
        case lfg::LFG_STATE_QUEUED:
            return DungeonQueueState::Queued;
        case lfg::LFG_STATE_PROPOSAL:
            return DungeonQueueState::Proposal;
        case lfg::LFG_STATE_BOOT:
            return DungeonQueueState::Boot;
        case lfg::LFG_STATE_DUNGEON:
            return DungeonQueueState::Dungeon;
        case lfg::LFG_STATE_FINISHED_DUNGEON:
            return DungeonQueueState::Finished;
        default:
            return DungeonQueueState::Other;
    }
}

bool Queue(ObjectGuid leaderGuid, ObjectGuid groupGuid, uint32 dungeonId, std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    if (!FinderEnabled())
    {
        why = "the Dungeon Finder is off";
        return false;
    }
    Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid);
    if (!leader || !leader->IsInWorld() || leader->IsDuringRemoveFromWorld() || !leader->GetSession() ||
        !leader->GetSession()->IsBot() || !GET_PLAYERBOT_AI(leader))
    {
        why = "the leader is not in the world";
        return false;
    }
    Group* group = leader->GetGroup();
    if (!group || group->GetGUID() != groupGuid || group->GetLeaderGUID() != leaderGuid)
    {
        why = "the leader does not lead the run's group";
        return false;
    }
    if (group->isLFGGroup() || group->isRaidGroup() || group->isBGGroup() || group->isBFGroup())
    {
        why = "the group is a Dungeon Finder, raid or battleground group";
        return false;
    }
    // LFGMgr::MakeNewGroup converts only a full premade group in place; a smaller one is dissolved into a new group
    // with the strangers the finder adds, and the run's groupGuid would be lost.
    if (group->GetMembersCount() != MAXGROUPSIZE)
    {
        why = "the group is not full (the finder would replace it with a new group)";
        return false;
    }
    DungeonQueueState const state = State(groupGuid);
    if (state != DungeonQueueState::None)
    {
        why = Acore::StringFormat("the group is at the Dungeon Finder already ({})", DungeonQueueStateName(state));
        return false;
    }
    if (!sLFGDungeonStore.LookupEntry(dungeonId) || !sLFGMgr->GetLFGDungeon(dungeonId))
    {
        why = Acore::StringFormat("unknown dungeon {}", dungeonId);
        return false;
    }

    lfg::LfgDungeonSet const dungeons{dungeonId};
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid);
        if (!member || !member->IsInWorld() || member->IsDuringRemoveFromWorld() || !member->GetSession())
        {
            why = "a member is offline or loading";
            return false;
        }
        if (member->GetSession()->IsBot())
            continue;
        // CoA challenge rules can forbid the finder to a character: then the run walks to the portal instead.
        lfg::LfgDungeonSet asked = dungeons;
        if (!sScriptMgr->OnPlayerCanJoinLfg(member, RoleMask(member), asked, ""))
        {
            why = "a player may not use the Dungeon Finder";
            return false;
        }
    }

    lfg::LfgDungeonSet join = dungeons;
    sLFGMgr->JoinLfg(leader, RoleMask(leader), join, "");
    leader->UpdateLFGChannel();

    DungeonQueueState const after = State(groupGuid);
    if (after == DungeonQueueState::RoleCheck || after == DungeonQueueState::Queued)
        return true;
    why = "the Dungeon Finder refused (see the leader's join result)";
    return false;
}

void Leave(ObjectGuid groupGuid)
{
    COA_ASSERT_WORLD_THREAD();
    DungeonQueueState const state = State(groupGuid);
    if (state == DungeonQueueState::RoleCheck || state == DungeonQueueState::Queued ||
        state == DungeonQueueState::Proposal)
    {
        LOG_DEBUG(LOG, "dungeon run: group {} leaves the Dungeon Finder ({})", groupGuid.GetCounter(),
                  DungeonQueueStateName(state));
        sLFGMgr->LeaveLfg(groupGuid);
    }
}

bool PrepareRole(Player* bot, Player* anchor, uint8 role, uint32 levelTolerance, std::string& why)
{
    COA_ASSERT_WORLD_THREAD();
    if (!bot || !anchor)
    {
        why = "no bot or no player to prepare it for";
        return false;
    }
    uint32 const specialization = GetAscensionActiveSpecialization(bot);
    bool const fits = specialization && !sPlayerbotAIConfig.coaExcludedSpecializations.count(specialization) &&
                      uint8(GetCoaRole(bot)) == role;
    if (!fits && !PrepareCoaRecruit(anchor, bot, CoaRole(role), false, levelTolerance, why))
        return false;
    CoaLeaveBattlegroundQueues(bot);
    CoaDropForeignInstanceBinds(bot, anchor);
    return true;
}

uint8 RoleOf(Player* bot) { return bot ? uint8(GetCoaRole(bot)) : uint8(CoaRole::Dps); }
}  // namespace DungeonLfg
