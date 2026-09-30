/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COATIME_H
#define PLAYERBOTS_COATIME_H

#include "Define.h"
#include <chrono>

namespace CoaTime
{
// Milliseconds on a steady clock, for delays and cooldowns. Never written to the database: use time(nullptr).
inline uint64 NowMs()
{
    return uint64(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace CoaTime

#endif
