/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICESANITIZER_H
#define PLAYERBOTS_COAVOICESANITIZER_H

#include "BrainTypes.h"
#include "Define.h"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class CoaVoiceReject : uint8
{
    None = 0,
    Empty,    // nothing left to say
    Blocked,  // a CoaVoice.BlockedWords word
    AiTalk,   // "as an AI", "en tant qu'IA", "modele de langage", provider or model names, "prompt" (untrusted)
    Url,      // http://, https://, www. (untrusted)
    Command,  // CoaChatIntent::LooksLikeCommand is true: bot-to-bot whispers would run it (untrusted)
    Fence,    // a prompt fence token such as "<<<" or ">>>" (untrusted)
    Invalid   // M6: Validate refused it: bad UTF-8, a control, format or 4-byte character, '|', '{', '}', a URL...
};

char const* CoaVoiceRejectName(CoaVoiceReject reason);  // "none", "empty", "blocked", "ai-talk", "url", "command",
                                                        // "fence", "invalid"

struct CoaVoiceSanitizeOptions
{
    uint32 maxBytes = 240;
    // Template rows are trusted. Model output (M6) is not: quotes, "Name:", markdown are stripped, '|' is
    // escaped as "||" so no link or colour can be forged, and the AiTalk/Url/Command/Fence checks run.
    bool untrusted = false;
    std::vector<std::string> const* blockedWords = nullptr;  // folded (NameMatch::Fold), may be null
    // M6. Skip CoaChatIntent::LooksLikeCommand (and Validate's): it reads sPlayerbotAIConfig, which .reload config
    // rewrites on the world thread, so a worker must not call it; the world thread checks it at emit time.
    bool skipCommandCheck = false;
    // M6. CleanGenerated only: "{name}" is allowed (the line is not filled in yet).
    bool allowNameToken = false;
};

// What makes a line safe to send, checked on the world thread at emit time and (minus the command test) on the
// worker. Stricter than Clean: a failure here means the line is not said.
struct CoaVoiceValidateOptions
{
    uint32 maxBytes = 240;
    std::vector<std::string> const* blockedWords = nullptr;  // folded, may be null
    bool checkCommand = true;     // CoaChatIntent::LooksLikeCommand; false on a worker
    bool allowNameToken = false;  // the token "{name}" (exactly) is allowed
};

// Makes a line safe to say. Pure function, any thread (the command test reads sPlayerbotAIConfig: workers skip it).
// The realm's own chat filter (sObjectMgr->IsChatFiltered) is not called here: it is rebuilt at runtime by
// .reload chat_filter, so the director checks it on the world thread at send time.
namespace CoaVoiceSanitizer
{
// Steps: keep the first line; drop control characters, invalid UTF-8 and 4-byte characters (the texts table is
// utf8mb3 and the 3.3.5 client shows no emoji); collapse spaces and trim; untrusted-only steps; blocked
// words; CoaText::CutUtf8Bytes to maxBytes without splitting a "||" pair; trim again. Returns false, with
// the reason, when the line must not be said; `line` is then unspecified.
bool Clean(std::string& line, CoaVoiceSanitizeOptions const& options, CoaVoiceReject* reason = nullptr);

// M6. True only for text that is safe to send: valid UTF-8 (RFC 3629, no overlong form, no surrogate), at most
// maxBytes, not empty, no '|' (so no link or colour code), no '{' or '}' (except the exact token "{name}" when
// allowed: no raid-target icon, no unfilled token), no control character (< 0x20, 0x7F, U+0080-U+009F, U+2028,
// U+2029), no 4-byte character, no zero-width, bidirectional or other format character (U+00AD, U+061C, U+180E,
// U+200B-U+200F, U+202A-U+202E, U+2060-U+2064, U+2066-U+206F, U+FEFF), no private-use or non-character code point,
// no run of more than 2 combining marks and none at the start or after a space, no URL ("://", "www.", a word
// that is a host name ending in a common TLD), not AI talk (as Clean), not blocked, and (checkCommand) not
// CoaChatIntent::LooksLikeCommand. The exact list is in the contract. Pure: any thread.
bool Validate(std::string_view line, CoaVoiceValidateOptions const& options, CoaVoiceReject* reason = nullptr);
bool Validate(std::string_view line);  // maxBytes 240, every check, no blocked words, no "{name}"

// M6. The text lane's output pipeline: Clean in untrusted mode (plus: format characters and combining-mark runs are
// dropped, no-break spaces become spaces), then Validate with the same limits (checkCommand = !skipCommandCheck,
// allowNameToken). True: `line` is the cleaned line; false: rejected, `reason` says why. The fuzz harness asserts
// that Validate(line) holds for every line this accepts, with checkCommand true once the token is filled in.
bool CleanGenerated(std::string& line, CoaVoiceSanitizeOptions const& options, CoaVoiceReject* reason = nullptr);
}  // namespace CoaVoiceSanitizer

// M6. The worker-side finisher of the voice lane and the banter: CleanGenerated with skipCommandCheck set, on
// immutable copies of the limits, so the worker reads no game state and no reloadable setting.
class CoaVoiceTextFinisher final : public BrainTextFinisher
{
public:
    CoaVoiceTextFinisher(uint32 maxBytes, std::vector<std::string> blockedWords, bool allowNameToken)
        : maxBytes(maxBytes), blockedWords(std::move(blockedWords)), allowNameToken(allowNameToken)
    {
    }

    bool Finish(std::string& line, std::string& reason) const override;

private:
    uint32 const maxBytes;
    std::vector<std::string> const blockedWords;
    bool const allowNameToken;
};

#endif
