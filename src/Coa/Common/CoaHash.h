/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAHASH_H
#define PLAYERBOTS_COAHASH_H

#include "Define.h"
#include <string_view>

// FNV-1a, 64 bits. Stable across platforms and restarts: personas and scope keys depend on it.
namespace CoaHash
{
constexpr uint64 FNV_OFFSET = 14695981039346656037ULL;
constexpr uint64 FNV_PRIME = 1099511628211ULL;

inline uint64 Bytes(std::string_view bytes, uint64 hash = FNV_OFFSET)
{
    for (char const c : bytes)
    {
        hash ^= uint64(static_cast<unsigned char>(c));
        hash *= FNV_PRIME;
    }
    return hash;
}

// The 8 bytes of value, least significant first, whatever the platform's byte order.
inline uint64 U64(uint64 value, uint64 hash = FNV_OFFSET)
{
    for (int i = 0; i < 8; ++i)
    {
        hash ^= (value >> (8 * i)) & 0xFF;
        hash *= FNV_PRIME;
    }
    return hash;
}
}  // namespace CoaHash

#endif
