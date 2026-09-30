/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MemoryView.h"
#include <algorithm>
#include <utility>

MemoryView::MemoryView(uint32 botGuid, uint64 epoch, uint32 builtAt, MemoryDecay const& decay,
                       std::vector<MemoryRelationView>&& relations)
    : botGuid(botGuid), epoch(epoch), builtAt(builtAt), decay(decay), relations(std::move(relations))
{
    std::sort(this->relations.begin(), this->relations.end(),
              [](MemoryRelationView const& a, MemoryRelationView const& b) { return a.otherGuid < b.otherGuid; });
}

MemoryRelationView const* MemoryView::Find(uint32 otherGuid) const
{
    auto const found =
        std::lower_bound(relations.begin(), relations.end(), otherGuid,
                         [](MemoryRelationView const& relation, uint32 guid) { return relation.otherGuid < guid; });
    return found != relations.end() && found->otherGuid == otherGuid ? &*found : nullptr;
}

int16 MemoryView::Axis(MemoryRelationView const& relation, MemoryAxis axis, uint32 now) const
{
    return decay.Apply(relation.axes[std::size_t(axis)], axis, relation.decayedAt, now);
}

std::array<int16, MEMORY_AXIS_COUNT> MemoryView::Axes(MemoryRelationView const& relation, uint32 now) const
{
    std::array<int16, MEMORY_AXIS_COUNT> axes{};
    for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
        axes[i] = decay.Apply(relation.axes[i], MemoryAxis(i), relation.decayedAt, now);
    return axes;
}

MemoryLabel MemoryView::Label(MemoryRelationView const& relation, uint32 now) const
{
    return MemoryLabelOf(Axes(relation, now), relation.counters, relation.lastKilledMeAt, relation.bgGrudgeUntil, now);
}
