/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */
#ifndef PLAYERBOTS_ACTIVITYCOMBAT_H
#define PLAYERBOTS_ACTIVITYCOMBAT_H
#include "ActivityKind.h"
namespace ActivityCombatRules
{
bool ReadyGroup(uint32 minimum, uint32 enrolled, uint32 nativeMembers, bool leaderIn, bool nativeLeader,
                bool pendingAdds);
bool CompletedPatrol(uint32 minimum, uint32 enrolled, uint32 nativeMembers, bool leaderIn, bool nativeLeader,
                     bool pendingAdds, bool patrolObserved);
bool SelfTest();
}
std::unique_ptr<ActivityKind> CreatePvpGangKind();
std::unique_ptr<ActivityKind> CreateBountyKind();
std::unique_ptr<ActivityKind> CreateRareCampKind();
#endif
