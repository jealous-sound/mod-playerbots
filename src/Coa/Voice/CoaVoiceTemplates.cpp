/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaVoiceTemplates.h"
#include "CoaThread.h"
#include "CoaVoicePersona.h"
#include "Random.h"
#include "SharedDefines.h"
#include <algorithm>
#include <initializer_list>

char const* CoaVoiceTemplates::ReplyName(CoaIntent intent, bool named)
{
    switch (intent)
    {
        case CoaIntent::Greeting:
            return "coa_reply_greet";
        case CoaIntent::Farewell:
            return "coa_reply_bye";
        case CoaIntent::Thanks:
            return "coa_reply_thanks";
        case CoaIntent::Question:
            return "coa_reply_question";
        case CoaIntent::AboutMe:
            return "coa_reply_about_me";
        case CoaIntent::Help:
            return "coa_reply_help";
        case CoaIntent::Lfg:
            return "coa_reply_lfg";
        case CoaIntent::Trade:
            return "coa_reply_trade";
        case CoaIntent::Insult:
            return "coa_reply_insult";
        case CoaIntent::Compliment:
            return "coa_reply_compliment";
        case CoaIntent::Joke:
            return "coa_reply_joke";
        case CoaIntent::Duel:
            return "coa_reply_duel";
        default:
            return named ? "coa_reply_name" : "coa_reply_generic";
    }
}

namespace
{
char const* TalkDenyName(DenyReason reason)
{
    switch (reason)
    {
        case PLAYERBOT_DENY_NOT_YOURS:
            return "coa_deny_taken";
        case PLAYERBOT_DENY_LOW_LEVEL:
        case PLAYERBOT_DENY_GEARSCORE:
            return "coa_deny_level";
        case PLAYERBOT_DENY_BG:
        case PLAYERBOT_DENY_LFG:
            return "coa_deny_queue";
        case PLAYERBOT_DENY_FULL_GROUP:
            return "coa_deny_full";
        case PLAYERBOT_DENY_NOT_LEADER:
            return "coa_deny_not_leader";
        default:
            return "coa_deny_busy";
    }
}
}  // namespace

char const* CoaVoiceTemplates::DenyName(PlayerbotSecurityLevel level, DenyReason reason)
{
    // Same wording choice as PlayerbotSecurity::CheckLevelFor: an INVITE-level refusal always asks for an
    // invitation, whatever the reason; a TALK-level one says why.
    switch (level)
    {
        case PLAYERBOT_SECURITY_DENY_ALL:
            return "coa_deny_busy";
        case PLAYERBOT_SECURITY_TALK:
            return TalkDenyName(reason);
        case PLAYERBOT_SECURITY_INVITE:
            return "coa_deny_invite";
        default:
            return nullptr;
    }
}

char const* CoaVoiceTemplates::EmoteName(CoaEmoteCategory category)
{
    switch (category)
    {
        case CoaEmoteCategory::Greet:
            return "coa_emote_greet";
        case CoaEmoteCategory::Bye:
            return "coa_emote_bye";
        case CoaEmoteCategory::Thank:
            return "coa_emote_thank";
        case CoaEmoteCategory::Affection:
            return "coa_emote_affection";
        case CoaEmoteCategory::Praise:
            return "coa_emote_praise";
        case CoaEmoteCategory::Cheer:
            return "coa_emote_cheer";
        case CoaEmoteCategory::Laugh:
            return "coa_emote_laugh";
        case CoaEmoteCategory::Rude:
            return "coa_emote_rude";
        case CoaEmoteCategory::Hurt:
            return "coa_emote_hurt";
        case CoaEmoteCategory::Gross:
            return "coa_emote_gross";
        case CoaEmoteCategory::Confused:
            return "coa_emote_confused";
        case CoaEmoteCategory::Sad:
            return "coa_emote_sad";
        case CoaEmoteCategory::Apology:
            return "coa_emote_apology";
        default:
            return nullptr;
    }
}

CoaEmoteCategory CoaVoiceTemplates::EmoteCategoryFor(uint32 textEmote)
{
    switch (textEmote)
    {
        case TEXT_EMOTE_WAVE:
        case TEXT_EMOTE_GREET:
        case TEXT_EMOTE_HAIL:
        case TEXT_EMOTE_HELLO:
        case TEXT_EMOTE_WELCOME:
        case TEXT_EMOTE_INTRODUCE:
        case TEXT_EMOTE_SALUTE:
            return CoaEmoteCategory::Greet;
        case TEXT_EMOTE_BYE:
            return CoaEmoteCategory::Bye;
        case TEXT_EMOTE_THANK:
            return CoaEmoteCategory::Thank;
        case TEXT_EMOTE_FLIRT:
        case TEXT_EMOTE_KISS:
        case TEXT_EMOTE_HUG:
        case TEXT_EMOTE_BLUSH:
        case TEXT_EMOTE_SMILE:
        case TEXT_EMOTE_LOVE:
        case TEXT_EMOTE_COMFORT:
        case TEXT_EMOTE_SOOTHE:
        case TEXT_EMOTE_PAT:
            return CoaEmoteCategory::Affection;
        case TEXT_EMOTE_AMAZE:
        case TEXT_EMOTE_APPLAUD:
        case TEXT_EMOTE_CLAP:
        case TEXT_EMOTE_CONGRATULATE:
        case TEXT_EMOTE_PRAISE:
        case TEXT_EMOTE_COMMEND:
        case TEXT_EMOTE_BOW:
        case TEXT_EMOTE_CURTSEY:
        case TEXT_EMOTE_KNEEL:
        case TEXT_EMOTE_FLEX:
        case TEXT_EMOTE_AGREE:
        case TEXT_EMOTE_NOD:
            return CoaEmoteCategory::Praise;
        case TEXT_EMOTE_CHEER:
        case TEXT_EMOTE_VICTORY:
        case TEXT_EMOTE_TOAST:
        case TEXT_EMOTE_DANCE:
        case TEXT_EMOTE_HAPPY:
        case TEXT_EMOTE_READY:
        case TEXT_EMOTE_GOODLUCK:
            return CoaEmoteCategory::Cheer;
        case TEXT_EMOTE_LAUGH:
        case TEXT_EMOTE_CHUCKLE:
        case TEXT_EMOTE_GIGGLE:
        case TEXT_EMOTE_GUFFAW:
        case TEXT_EMOTE_ROFL:
        case TEXT_EMOTE_SNICKER:
        case TEXT_EMOTE_CACKLE:
        case TEXT_EMOTE_JOKE:
        case TEXT_EMOTE_JK:
            return CoaEmoteCategory::Laugh;
        case TEXT_EMOTE_RUDE:
        case TEXT_EMOTE_INSULT:
        case TEXT_EMOTE_CHICKEN:
        case TEXT_EMOTE_ANGRY:
        case TEXT_EMOTE_GLARE:
        case TEXT_EMOTE_BLAME:
        case TEXT_EMOTE_SHOO:
        case TEXT_EMOTE_SPIT:
        case TEXT_EMOTE_GROWL:
        case TEXT_EMOTE_THREATEN:
            return CoaEmoteCategory::Rude;
        case TEXT_EMOTE_SLAP:
        case TEXT_EMOTE_BITE:
        case TEXT_EMOTE_POKE:
        case TEXT_EMOTE_SCRATCH:
        case TEXT_EMOTE_TICKLE:
            return CoaEmoteCategory::Hurt;
        case TEXT_EMOTE_FART:
        case TEXT_EMOTE_BURP:
        case TEXT_EMOTE_NOSEPICK:
        case TEXT_EMOTE_SNIFF:
        case TEXT_EMOTE_STINK:
        case TEXT_EMOTE_COUGH:
        case TEXT_EMOTE_DROOL:
        case TEXT_EMOTE_LICK:
        case TEXT_EMOTE_BREATH:
            return CoaEmoteCategory::Gross;
        case TEXT_EMOTE_CONFUSED:
        case TEXT_EMOTE_CURIOUS:
        case TEXT_EMOTE_FROWN:
        case TEXT_EMOTE_STARE:
        case TEXT_EMOTE_BOGGLE:
        case TEXT_EMOTE_LOST:
        case TEXT_EMOTE_PONDER:
        case TEXT_EMOTE_EYEBROW:
        case TEXT_EMOTE_BLINK:
        case TEXT_EMOTE_SHRUG:
        case TEXT_EMOTE_TALKQ:
        case TEXT_EMOTE_SURPRISED:
        case TEXT_EMOTE_GASP:
            return CoaEmoteCategory::Confused;
        case TEXT_EMOTE_CRY:
        case TEXT_EMOTE_MOURN:
        case TEXT_EMOTE_BLEED:
        case TEXT_EMOTE_BEG:
        case TEXT_EMOTE_GROVEL:
        case TEXT_EMOTE_PLEAD:
        case TEXT_EMOTE_PITY:
            return CoaEmoteCategory::Sad;
        case TEXT_EMOTE_APOLOGIZE:
            return CoaEmoteCategory::Apology;
        default:
            return CoaEmoteCategory::None;
    }
}

ChatReplyType CoaVoiceTemplates::LegacyReplyType(CoaIntent intent, bool named)
{
    if (intent == CoaIntent::Greeting)
        return REPLY_HELLO;
    if (intent == CoaIntent::Insult)
        return REPLY_GRUDGE;
    return named ? REPLY_NAME : REPLY_NOT_UNDERSTAND;
}

uint32 CoaVoiceTemplates::ReplyEmoteFor(CoaIntent intent, CoaVoicePersona const& persona)
{
    uint32 choices[4] = {};
    uint32 count = 0;
    auto const add = [&](std::initializer_list<uint32> emotes)
    {
        for (uint32 const emote : emotes)
            choices[count++] = emote;
    };

    switch (intent)
    {
        case CoaIntent::Greeting:
            add({TEXT_EMOTE_WAVE, TEXT_EMOTE_HELLO, TEXT_EMOTE_SALUTE, TEXT_EMOTE_BOW});
            break;
        case CoaIntent::Farewell:
            add({TEXT_EMOTE_WAVE, TEXT_EMOTE_BYE});
            break;
        case CoaIntent::Thanks:
            add({TEXT_EMOTE_BOW, TEXT_EMOTE_NOD, TEXT_EMOTE_SMILE});
            break;
        case CoaIntent::Compliment:
            add({TEXT_EMOTE_BLUSH, TEXT_EMOTE_BOW, TEXT_EMOTE_THANK});
            break;
        case CoaIntent::Joke:
            add({TEXT_EMOTE_LAUGH, TEXT_EMOTE_CHUCKLE, TEXT_EMOTE_GIGGLE});
            break;
        case CoaIntent::Insult:
            add({TEXT_EMOTE_SHRUG, TEXT_EMOTE_SIGH});
            if (persona.formality < 0.4f)
                add({TEXT_EMOTE_RUDE});
            break;
        case CoaIntent::Duel:
            add({TEXT_EMOTE_FLEX, TEXT_EMOTE_SHRUG});
            break;
        default:
            return 0;
    }

    // A favourite, in the persona's order, keeps a bot's manner the same from one answer to the next.
    for (uint32 const favourite : persona.favouriteEmotes)
        if (std::find(choices, choices + count, favourite) != choices + count)
            return favourite;

    return choices[urand(0, count - 1)];
}

CoaVoiceTemplates::Placeholders& CoaVoiceTemplates::Placeholders::Set(char const* token, std::string value)
{
    if (!token || !*token)
        return *this;

    for (auto& [name, current] : values)
    {
        if (name == token)
        {
            current = std::move(value);
            return *this;
        }
    }

    // Longest first, so "%spec" is never read as "%s" followed by "pec".
    std::string name(token);
    auto const at =
        std::find_if(values.begin(), values.end(), [&](auto const& entry) { return entry.first.size() < name.size(); });
    values.emplace(at, std::move(name), std::move(value));
    return *this;
}

std::string CoaVoiceTemplates::Placeholders::Fill(std::string text) const
{
    if (values.empty() || text.find('%') == std::string::npos)
        return text;

    // One pass, so a value is never searched for tokens itself.
    std::string out;
    out.reserve(text.size() + 32);
    std::size_t i = 0;
    while (i < text.size())
    {
        if (text[i] != '%')
        {
            std::size_t const next = text.find('%', i);
            std::size_t const end = next == std::string::npos ? text.size() : next;
            out.append(text, i, end - i);
            i = end;
            continue;
        }

        bool replaced = false;
        for (auto const& [token, value] : values)
        {
            if (text.compare(i, token.size(), token) == 0)
            {
                out += value;
                i += token.size();
                replaced = true;
                break;
            }
        }
        if (!replaced)
            out += text[i++];
    }
    return out;
}

std::vector<std::string> CoaVoiceTemplates::Candidates(std::string const& name, uint32 locale, uint32 maxCount)
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<std::string> out;
    PlayerbotTextMgr::instance().GetCandidates(name, locale, maxCount, out);
    return out;
}

std::vector<std::string> CoaVoiceTemplates::Candidates(ChatReplyType replyType, uint32 locale, uint32 maxCount)
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<std::string> out;
    PlayerbotTextMgr::instance().GetCandidates(replyType, locale, maxCount, out);
    return out;
}
