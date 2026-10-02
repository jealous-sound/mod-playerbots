/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COACHATINTENT_H
#define PLAYERBOTS_COACHATINTENT_H

#include "Define.h"
#include <string_view>

// What a line of chat asks for, from French and English keywords. The order is the order Classify tests them.
enum class CoaIntent : uint8
{
    None = 0,    // nothing to answer (empty)
    Insult,      // "noob", "t'es nul", "stfu"
    Thanks,      // "merci", "thx"
    Lfg,         // "qui pour", "cherche groupe"
    Trade,       // "vends", "achete", "prix"
    Duel,        // "duel", "1v1"
    Help,        // "aide", "tu peux m'aider", "can you"
    AboutMe,     // "quelle classe", "ton niveau", "what class"
    Greeting,    // "salut", "bonjour", "hello"
    Farewell,    // "bye", "a plus", "bonne nuit"
    Compliment,  // "bravo", "gg", "bien joue"
    Joke,        // "lol", "mdr", "haha"
    Question,    // ends with '?' or starts with a question word
    Other,       // anything else
    Count
};

namespace CoaChatIntent
{
// Whether the text is a bot command rather than conversation. Pure and cheap; any thread. Built from the
// module's own command knowledge: ChatTriggerContext trigger names (whole text or a leading run of words,
// like ExternalEventHelper::ParseChatCommand), PlayerbotAI::GetUnsecuredCommands, the words HandleCommand
// handles inline (do, d, queue, reset, logout, debug), the no-reply lists of ChatReplyAction, the
// #w/#p/#r/#a/#g prefixes, a leading '@' (chat filters), '.' or '!', CommandPrefix, CommandSeparator and
// strategy syntax (+x, -x, ~x first, or after co/nc/de/react). Trigger names that are everyday words ("de",
// "go", "chat", "help", "talk", "ready", "t", "c", "e", "s", "u", "b", "r", "q") count only when they are the
// whole text or are followed by strategy syntax or '?'.
bool LooksLikeCommand(std::string_view text);

// Whether ExternalEventHelper::ParseChatCommand would find a trigger for the text: the whole text or a leading run
// of words, trimmed, compared case-sensitively (HandleCommand queues the text as written), without the everyday-
// word exception of LooksLikeCommand ("s gray", "help me" and "c est bon" all dispatch). False when a
// CommandPrefix is configured: prefixed text is already a command for LooksLikeCommand. Pure; any thread.
bool DispatchesAsTrigger(std::string_view text);

// Whether the text holds a hyperlink (|H...): item, quest, spell or achievement links.
bool HasLink(std::string_view text);

// Whether the voice layer may answer the text: at least 2 letters (or the whole text "a+", "à+" or "++"), no
// link, not a command, not a market or group post owned by the legacy replies and CoaLfg (first word lfg, lfm,
// lf, wtb, wts, wtt), not a '.' or '!' keyword.
bool IsConversation(std::string_view text);

// The intent of a conversation line (see CoaIntent for the order). Pure; any thread.
CoaIntent Classify(std::string_view text);

// "greeting", "thanks"...: for logs and .playerbots voice stats.
char const* IntentName(CoaIntent intent);
}  // namespace CoaChatIntent

#endif
