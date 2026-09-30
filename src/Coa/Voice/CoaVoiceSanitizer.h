/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICESANITIZER_H
#define PLAYERBOTS_COAVOICESANITIZER_H

#include "Define.h"
#include <string>
#include <vector>

enum class CoaVoiceReject : uint8
{
    None = 0,
    Empty,    // nothing left to say
    Blocked,  // a CoaVoice.BlockedWords word
    AiTalk,   // "as an AI", "en tant qu'IA", "modele de langage", provider or model names, "prompt" (untrusted)
    Url,      // http://, https://, www. (untrusted)
    Command,  // CoaChatIntent::LooksLikeCommand is true: bot-to-bot whispers would run it (untrusted)
    Fence     // a prompt fence token such as "<<<" or ">>>" (untrusted)
};

struct CoaVoiceSanitizeOptions
{
    uint32 maxBytes = 240;
    // Template rows are trusted. Model output (M6) is not: quotes, "Name:", markdown are stripped, '|' is
    // escaped as "||" so no link or colour can be forged, and the AiTalk/Url/Command/Fence checks run.
    bool untrusted = false;
    std::vector<std::string> const* blockedWords = nullptr;  // folded (NameMatch::Fold), may be null
};

// Makes a line safe to say. Pure function, any thread. The realm's own chat filter (sObjectMgr->IsChatFiltered)
// is not called here: it is rebuilt at runtime by .reload chat_filter, so the director checks it on the world
// thread at send time.
namespace CoaVoiceSanitizer
{
// Steps: keep the first line; drop control characters, invalid UTF-8 and 4-byte characters (the texts table is
// utf8mb3 and the 3.3.5 client shows no emoji); collapse spaces and trim; untrusted-only steps; blocked
// words; CoaText::CutUtf8Bytes to maxBytes without splitting a "||" pair; trim again. Returns false, with
// the reason, when the line must not be said; `line` is then unspecified.
bool Clean(std::string& line, CoaVoiceSanitizeOptions const& options, CoaVoiceReject* reason = nullptr);
}  // namespace CoaVoiceSanitizer

#endif
