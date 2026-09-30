/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaText.h"

namespace
{
bool IsUtf8Continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
}  // namespace

void CoaText::CutUtf8Bytes(std::string& text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return;

    // A byte of the cut position that continues a character means that character starts before it: cut at its
    // first byte instead. A UTF-8 character is at most 4 bytes long, so text that is not UTF-8 backs up 3 at most.
    std::size_t cut = maxBytes;
    std::size_t const lowest = maxBytes > 3 ? maxBytes - 3 : 0;
    while (cut > lowest && IsUtf8Continuation(text[cut]))
        --cut;

    text.resize(cut);
}
