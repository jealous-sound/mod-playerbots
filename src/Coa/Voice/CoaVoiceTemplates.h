/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICETEMPLATES_H
#define PLAYERBOTS_COAVOICETEMPLATES_H

#include "CoaChatIntent.h"
#include "Define.h"
#include "PlayerbotSecurity.h"
#include "PlayerbotTextMgr.h"
#include <string>
#include <utility>
#include <vector>

struct CoaVoicePersona;

// What a text emote aimed at a bot is about: picks the coa_emote_* row the bot answers with.
enum class CoaEmoteCategory : uint8
{
    None = 0,  // no verbal answer
    Greet,
    Bye,
    Thank,
    Affection,
    Praise,
    Cheer,
    Laugh,
    Rude,
    Hurt,
    Gross,
    Confused,
    Sad,
    Apology,
    Count
};

// Placeholders the coa_* rows may hold. None is the beginning of another, so the order of replacement does not
// matter. Legacy "reply" rows use "%s" for the speaker's name.
namespace CoaVoicePlaceholder
{
constexpr char const* PLAYER = "%player";           // the real player spoken to
constexpr char const* BOT = "%bot";                 // the bot's own name
constexpr char const* CLASS = "%class";             // the bot's class (CoaClassName, English)
constexpr char const* SPEC = "%spec";               // the bot's specialization, or its class
constexpr char const* LEVEL = "%level";             // the bot's level
constexpr char const* THEIR_LEVEL = "%theirlevel";  // the player's level
constexpr char const* ZONE = "%zone";               // the bot's zone (server DBC locale)
constexpr char const* LEADER = "%leader";           // the leader of the bot's group
constexpr char const* TOPIC = "%topic";             // an opt-out topic word (commands)
constexpr char const* LEGACY_NAME = "%s";           // legacy "reply" rows: the speaker's name
}  // namespace CoaVoicePlaceholder

// Text names, placeholder filling and candidate lines. The name tables are pure (any thread); Candidates reads
// PlayerbotTextMgr and must run on the world thread (".bot reload" appends to the same maps there).
namespace CoaVoiceTemplates
{
// "coa_reply_greet"... for an intent. `named` (the bot's name is in the text) turns Other into coa_reply_name.
// Never null.
char const* ReplyName(CoaIntent intent, bool named);

// The coa_deny_* row for a refusal, from PlayerbotSecurity::LevelFor. Null when there is nothing to refuse
// (level ALLOW_ALL). DENY_ALL level gives coa_deny_busy, as the legacy refusal does.
char const* DenyName(PlayerbotSecurityLevel level, DenyReason reason);

// "coa_emote_greet"... Null for None.
char const* EmoteName(CoaEmoteCategory category);

// The category of a text emote id (TextEmotes), None when a bot has nothing to say to it. Pure table.
CoaEmoteCategory EmoteCategoryFor(uint32 textEmote);

// The legacy "reply" row type used when no coa_reply_* row exists for the intent.
ChatReplyType LegacyReplyType(CoaIntent intent, bool named);

// The TextEmotes id a bot may play with an answer of that intent (a favourite of the persona when one fits),
// 0 for none. Uses urand: world thread.
uint32 ReplyEmoteFor(CoaIntent intent, CoaVoicePersona const& persona);

class Placeholders
{
public:
    Placeholders& Set(char const* token, std::string value);
    // Every token set here replaced in `text`; tokens not set are left as they are.
    std::string Fill(std::string text) const;

private:
    std::vector<std::pair<std::string, std::string>> values;
};

// World thread. At most maxCount distinct lines of that text name in `locale`, in random order, unfilled.
// Rows with a text_loc<locale> are used when there is at least one; English (`text`) otherwise.
std::vector<std::string> Candidates(std::string const& name, uint32 locale, uint32 maxCount);

// World thread. The same over the legacy "reply" rows of that reply type.
std::vector<std::string> Candidates(ChatReplyType replyType, uint32 locale, uint32 maxCount);
}  // namespace CoaVoiceTemplates

#endif
