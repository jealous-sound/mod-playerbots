/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COATEXT_H
#define PLAYERBOTS_COATEXT_H

#include <cstddef>
#include <string>

namespace CoaText
{
// Cuts text to at most maxBytes bytes without leaving the first bytes of a UTF-8 character at the end.
// The core's utf8truncate counts characters, not bytes, so it cannot keep a chat line under a byte limit.
void CutUtf8Bytes(std::string& text, std::size_t maxBytes);
}  // namespace CoaText

#endif
