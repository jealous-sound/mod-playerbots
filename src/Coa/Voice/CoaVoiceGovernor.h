/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICEGOVERNOR_H
#define PLAYERBOTS_COAVOICEGOVERNOR_H

#include "CoaVoiceEvent.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

struct CoaVoiceConfig;

// Anti-spam state: cooldowns, rates, the repetition filter and the "a real player spoke here" clock. Owned by
// the director and used on the world thread only, so it takes no lock. An answer is reserved when it is
// decided (not when it is sent 4-12 s later), so two messages in a row cannot both pass the same cooldown.
class CoaVoiceGovernor
{
public:
    // Whether `bot` may answer `sender` in `scope` now. No state change. Checks, in this order: GlobalRate,
    // BotCooldown (cooldownWhisperMs for a whisper, cooldownBotMs otherwise), ScopeCooldown (cooldownWhisperMs
    // for a whisper scope, cooldownScopeMs otherwise), ScopeRate, SenderCooldown (public scopes only).
    // Returns CoaVoiceDrop::Count when every check passes.
    CoaVoiceDrop PreCheck(CoaVoiceConfig const& config, ObjectGuid bot, CoaVoiceScope const& scope, ObjectGuid sender,
                          uint64 nowMs) const;

    // Whether `line` repeats what `bot` said lately (its last 10 lines) or what was said in the scope (its last
    // repetitionHistory lines), within 30 minutes: same normalized text, word Jaccard >= repetitionThreshold,
    // or an opening (first two words) already used twice in the scope's last 10 lines.
    bool IsRepeat(CoaVoiceConfig const& config, ObjectGuid bot, uint64 scopeKey, std::string const& line,
                  uint64 nowMs) const;

    // Records an answer decided now: every cooldown, rate and history above starts counting it.
    void Reserve(CoaVoiceConfig const& config, ObjectGuid bot, CoaVoiceScope const& scope, ObjectGuid sender,
                 std::string const& line, uint64 nowMs);

    // M6. Records `line` in the bot's and the scope's repetition history at nowMs, with no cooldown, rate or sender
    // effect. A text line is reserved with an empty line (Reserve) when it is decided, and its real text (the
    // generated line, or the template that replaced it) is noted here when it is sent.
    void NoteLine(ObjectGuid bot, uint64 scopeKey, std::string const& line, uint64 nowMs);

    // A real player spoke in that scope (every human event notes it).
    void NoteHumanSpoke(uint64 scopeKey, uint64 nowMs);
    bool HumanSpokeWithin(uint64 scopeKey, uint64 nowMs, uint64 windowMs) const;

    // Forgets bots, speakers and scopes idle for 30 minutes, and the oldest scopes beyond 512. Call it about
    // once a minute.
    void Prune(uint64 nowMs);

    // Everything forgotten (voice turned off, shutdown).
    void Reset();

    // When `bot` last had an answer reserved (whisper or public), 0 for never. The director's score uses it.
    uint64 LastAnswerMs(ObjectGuid bot) const;

    uint32 TrackedScopes() const { return uint32(scopes.size()); }
    uint32 RepliesLastMinute(uint64 nowMs) const;

private:
    struct RecentLine
    {
        uint64 atMs = 0;
        std::string normalized;          // folded words joined by one space
        std::vector<std::string> words;  // the same words, sorted and unique, for the Jaccard test
    };

    struct BotState
    {
        uint64 lastPublicMs = 0;
        uint64 lastWhisperMs = 0;
        std::deque<RecentLine> lines;  // last 10
    };

    struct ScopeState
    {
        uint64 lastReplyMs = 0;
        uint64 lastHumanMs = 0;
        uint64 touchedMs = 0;
        std::deque<uint64> repliesMs;  // within the last minute
        std::deque<RecentLine> lines;  // last max(repetitionHistory, 10)
    };

    std::unordered_map<ObjectGuid, BotState> bots;
    std::unordered_map<uint64, ScopeState> scopes;
    std::unordered_map<ObjectGuid, uint64> senders;  // last public answer to that speaker
    std::deque<uint64> globalMs;                     // answers within the last minute
};

#endif
