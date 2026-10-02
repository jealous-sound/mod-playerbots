/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAEPOCH_H
#define PLAYERBOTS_COAEPOCH_H

#include "Define.h"
#include <atomic>

// One process-wide counter. Every PlayerbotAI takes the next value in its constructor, so a bot that logs out and
// back in never gets the same epoch twice, even within one tick. Events and snapshots carry the epoch of the bot
// life they belong to, and the world thread drops whatever belongs to an older life. Never 0: 0 means "no bot".
namespace CoaEpoch
{
inline std::atomic<uint64> Last{0};

// Any thread.
inline uint64 Next() { return Last.fetch_add(1, std::memory_order_relaxed) + 1; }
}  // namespace CoaEpoch

#endif
