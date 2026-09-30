/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COATHREAD_H
#define PLAYERBOTS_COATHREAD_H

#include "Errors.h"

// The world thread runs World::Update: real players' chat, then (after the map threads are joined) the
// WorldScript OnUpdate hooks. The CoA layers do all their game work there, and assert it in debug builds.
namespace CoaThread
{
// Records the calling thread as the world thread; the first call wins. CoaWorldScript calls it from
// OnAfterConfigLoad, OnStartup and every OnUpdate, which all run on the thread that runs World::Update.
void CaptureWorldThread();

// True only on the captured world thread.
bool IsWorldThread();

// True on the captured world thread, and on every thread before the capture (nothing to compare with yet).
bool IsWorldThreadOrUnknown();
}  // namespace CoaThread

// Debug builds (ACORE_DEBUG), or any build compiled with -DCOA_THREAD_CHECKS (a TSan run, for instance).
#if defined(ACORE_DEBUG) || defined(COA_THREAD_CHECKS)
#define COA_ASSERT_WORLD_THREAD() ASSERT(CoaThread::IsWorldThreadOrUnknown(), "CoA: world thread only")
#else
#define COA_ASSERT_WORLD_THREAD() ((void)0)
#endif

#endif
