/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaThread.h"
#include <atomic>
#include <thread>

namespace
{
// A default-constructed id means "no thread": nothing was captured yet.
std::atomic<std::thread::id> worldThread{};
}  // namespace

void CoaThread::CaptureWorldThread()
{
    std::thread::id const self = std::this_thread::get_id();
    std::thread::id captured = worldThread.load(std::memory_order_acquire);
    if (captured == std::thread::id() &&
        worldThread.compare_exchange_strong(captured, self, std::memory_order_acq_rel, std::memory_order_acquire))
        return;

    // Every caller runs on the thread of World::Update; another thread here means a hook moved threads.
#if defined(ACORE_DEBUG) || defined(COA_THREAD_CHECKS)
    ASSERT(captured == self, "CoA: the world thread was captured from another thread");
#endif
}

bool CoaThread::IsWorldThread() { return worldThread.load(std::memory_order_acquire) == std::this_thread::get_id(); }

bool CoaThread::IsWorldThreadOrUnknown()
{
    std::thread::id const world = worldThread.load(std::memory_order_acquire);
    return world == std::thread::id() || world == std::this_thread::get_id();
}
