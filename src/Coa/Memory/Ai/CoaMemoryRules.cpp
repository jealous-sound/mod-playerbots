/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaMemoryRules.h"
#include "CoaPlayerPrefs.h"
#include "Log.h"
#include "MemoryView.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "WorldSession.h"
#include <algorithm>
#include <ctime>

namespace
{
// Fled duels count against a player for a day: someone who rode away from two challenges is not locked out of
// duels until the events expire. A clock set back counts as expired, never as a lock.
bool FledRecently(MemoryRelationView const& relation, uint32 now)
{
    return relation.fledStreak >= MemoryThreshold::DUEL_REFUSE_FLED && now >= relation.lastFledAt &&
           now - relation.lastFledAt <= MemoryThreshold::DUEL_FLED_WINDOW;
}

char const* VerdictName(CoaDuelVerdict verdict)
{
    switch (verdict)
    {
        case CoaDuelVerdict::Eager:
            return "eager";
        case CoaDuelVerdict::Refuse:
            return "refuse";
        default:
            return "legacy";
    }
}
}  // namespace

std::shared_ptr<MemoryView const> CoaMemoryRules::ViewFor(PlayerbotAI* botAI, MemoryBehaviour behaviour)
{
    // Memory off, or this switch off: one relaxed load, and the caller runs its legacy code.
    if (!CoaMemoryConfig::BehaviourActive(behaviour))
        return nullptr;

    if (!botAI || !botAI->HasStrategy("coa memory", BOT_STATE_NON_COMBAT))
        return nullptr;

    std::shared_ptr<MemoryView const> view = botAI->GetMemoryView();
    // The manager delivers only views of this life of the bot; the test keeps a stale one out all the same.
    if (view && view->Epoch() != botAI->GetCoaEpoch())
        return nullptr;

    return view;
}

CoaDuelVerdict CoaMemoryRules::DuelVerdict(PlayerbotAI* botAI, ObjectGuid challenger)
{
    std::shared_ptr<MemoryView const> const view = ViewFor(botAI, MemoryBehaviour::RivalDuels);
    if (!view)
        return CoaDuelVerdict::Legacy;

    Player* master = botAI->GetMaster();
    if (master && master->GetGUID() == challenger)
        return CoaDuelVerdict::Legacy;

    MemoryRelationView const* relation = view->Find(challenger.GetCounter());
    if (!relation)
        return CoaDuelVerdict::Legacy;

    uint32 const now = uint32(time(nullptr));
    CoaDuelVerdict verdict = CoaDuelVerdict::Legacy;
    if (view->Axis(*relation, MemoryAxis::Affinity, now) <= MemoryThreshold::DUEL_REFUSE_AFFINITY ||
        view->Axis(*relation, MemoryAxis::Annoyance, now) >= MemoryThreshold::DUEL_REFUSE_ANNOYANCE ||
        FledRecently(*relation, now))
        verdict = CoaDuelVerdict::Refuse;
    else
    {
        MemoryLabel const label = view->Label(*relation, now);
        if (label == MemoryLabel::Rival || label == MemoryLabel::Grudge || label == MemoryLabel::Friend)
            verdict = CoaDuelVerdict::Eager;
    }

    LOG_DEBUG("playerbots.memory", "bot {} duel from {}: {}", view->BotGuid(), challenger.GetCounter(),
              VerdictName(verdict));
    return verdict;
}

int32 CoaMemoryRules::RevengeScore(MemoryView const& view, Player* target, uint32 now)
{
    if (!target)
        return 0;

    MemoryRelationView const* relation = view.Find(target->GetGUID().GetCounter());
    if (!relation || view.Label(*relation, now) != MemoryLabel::Grudge)
        return 0;

    if (!relation->counters[std::size_t(MemoryCounter::KilledMe)] && relation->bgGrudgeUntil <= now)
        return 0;

    // Preferences not loaded yet (memory just turned on by a reload): an opt-out could not be seen, so no revenge.
    WorldSession* session = target->GetSession();
    uint32 const account = session ? session->GetAccountId() : 0;
    CoaPlayerPrefs const& prefs = CoaPlayerPrefs::Instance();
    if (account && (!prefs.IsLoaded() || (prefs.Get(account).activityMask & COA_OPTOUT_HOSTILE)))
        return 0;

    // A grudge label may come from a very low affinity alone: still a target, with the lowest score.
    return std::max<int32>(1, view.Axis(*relation, MemoryAxis::Grudge, now));
}
