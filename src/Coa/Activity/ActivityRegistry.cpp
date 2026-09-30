/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityRegistry.h"
#include "CoaThread.h"
#include <utility>

ActivityRegistry& ActivityRegistry::Instance()
{
    static ActivityRegistry instance;
    return instance;
}

ActivityRegistry::ActivityRegistry() : current(std::make_shared<ActivityRegistrySnapshot const>()) {}

void ActivityRegistry::Publish(std::shared_ptr<ActivityRegistrySnapshot> snapshot)
{
    COA_ASSERT_WORLD_THREAD();
    if (!snapshot)
        snapshot = std::make_shared<ActivityRegistrySnapshot>();

    uint64 const next = version.load(std::memory_order_relaxed) + 1;
    snapshot->version = next;
    bool const held = !snapshot->members.empty() || !snapshot->reserved.empty();

    std::shared_ptr<ActivityRegistrySnapshot const> previous;
    {
        std::lock_guard<std::mutex> guard(lock);
        previous = std::move(current);
        current = std::move(snapshot);
    }
    // Readers that see the new version (acquire) find the new snapshot under the lock. The previous snapshot is
    // released here, outside the lock (or later, by the last reader holding it).
    anyHeld.store(held, std::memory_order_relaxed);
    version.store(next, std::memory_order_release);
}

std::shared_ptr<ActivityRegistrySnapshot const> ActivityRegistry::Current() const
{
    std::lock_guard<std::mutex> guard(lock);
    return current;
}

bool ActivityRegistry::IsParticipant(uint32 botLow)
{
    if (!AnyHeld())
        return false;
    return Instance().Current()->members.count(botLow) != 0;
}

bool ActivityRegistry::IsHeld(uint32 botLow)
{
    if (!AnyHeld())
        return false;
    return Instance().Current()->IsHeld(botLow);
}

bool ActivityRegistry::IsActivityGroup(ObjectGuid groupGuid)
{
    if (!AnyHeld() || groupGuid.IsEmpty())
        return false;
    return Instance().Current()->groups.count(groupGuid.GetCounter()) != 0;
}

std::optional<ActivityMemberView> ActivityRegistry::Find(uint32 botLow)
{
    if (!AnyHeld())
        return std::nullopt;
    std::shared_ptr<ActivityRegistrySnapshot const> const snapshot = Instance().Current();
    if (ActivityMemberView const* row = snapshot->Find(botLow))
        return *row;
    return std::nullopt;
}
