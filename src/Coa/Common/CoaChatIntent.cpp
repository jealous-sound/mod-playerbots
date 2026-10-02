/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaChatIntent.h"
#include "ChatTriggerContext.h"
#include "NameMatch.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
// Mirror of ChatReplyAction's lists (SayAction.cpp): texts its generic reply never answers.
// clang-format off
char const* const NO_REPLY_MSGS[] = {
    "join", "leave", "follow", "attack", "pull", "flee", "reset", "reset ai", "all ?", "talents", "talents list",
    "talents auto", "talk", "stay", "stats", "who", "items", "repair", "summon", "nc ?", "co ?", "de ?", "dead ?",
    "los", "guard", "do accept invitation", "react ?", "reset strats", "home"};
// clang-format on
// The bare "+" and "-" of ChatReplyAction's noReplyMsgParts are left out: strategy syntax covers them.
char const* const NO_REPLY_MSG_PARTS[] = {"@",        "follow target", "focus heal", "cast ",
                                          "accept [", "e [",           "destroy [",  "go zone"};
std::string_view const NO_REPLY_MSG_STARTS[] = {"e ", "accept ", "cast ", "destroy "};

// Chat trigger names that are also everyday words: a command only alone, or before strategy syntax or '?'.
std::string_view const EVERYDAY_TRIGGERS[] = {"de", "go", "chat", "help", "talk", "ready", "t",
                                              "c",  "e",  "s",    "u",    "b",    "r",     "q"};

// HandleCommand's own words, tested before any trigger (PlayerbotAI::HandleCommand).
std::string_view const INLINE_EXACT[] = {"reset", "logout", "logout cancel"};
std::string_view const INLINE_STARTS[] = {"do ", "d ", "queue ", "debug "};
std::string_view const CHAT_PREFIXES[] = {"#w ", "#p ", "#r ", "#a ", "#g "};
std::string_view const STRATEGY_OWNERS[] = {"co", "nc", "de", "react"};
std::string_view const MARKET_WORDS[] = {"lfg", "lfm", "lf", "wtb", "wts", "wtt"};

using TriggerSet = std::set<std::string, std::less<>>;

struct CommandVocabulary
{
    TriggerSet triggers;       // lower-case
    TriggerSet exactTriggers;  // as ChatTriggerContext names them: ParseChatCommand looks them up case-sensitively
    std::size_t longestTrigger = 0;
    std::set<std::string, std::less<>> noReplyMsgs;
};

// Built once (function-local static: thread-safe initialisation), read-only afterwards, so the map-thread hooks
// may call it. ChatTriggerContext only fills its creator table here: no trigger is created.
CommandVocabulary const& Commands()
{
    static CommandVocabulary const vocabulary = []
    {
        CommandVocabulary built;
        ChatTriggerContext context;
        for (std::string const& name : context.supports())
        {
            built.exactTriggers.insert(name);
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; });
            built.longestTrigger = std::max(built.longestTrigger, lower.size());
            built.triggers.insert(std::move(lower));
        }
        for (char const* msg : NO_REPLY_MSGS)
            built.noReplyMsgs.insert(msg);
        return built;
    }();
    return vocabulary;
}

template <std::size_t N>
bool IsOneOf(std::string_view word, std::string_view const (&list)[N])
{
    return std::find(std::begin(list), std::end(list), word) != std::end(list);
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && IsSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

std::string LowerAscii(std::string_view text)
{
    std::string lower(text);
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return lower;
}

bool IsAsciiLetter(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// ASCII letters plus multi-byte characters (counted once, by their lead byte), up to `enough`.
std::size_t CountLetters(std::string_view text, std::size_t enough)
{
    std::size_t letters = 0;
    for (char const c : text)
    {
        unsigned char const byte = static_cast<unsigned char>(c);
        if (IsAsciiLetter(byte) || byte >= 0xC0)
            if (++letters >= enough)
                break;
    }
    return letters;
}

// "+dps", "-aoe", "~tank": a strategy change.
bool IsStrategyToken(std::string_view word)
{
    return word.size() >= 2 && (word[0] == '+' || word[0] == '-' || word[0] == '~') &&
           IsAsciiLetter(static_cast<unsigned char>(word[1]));
}

std::string_view WordAt(std::string_view t, std::size_t& pos)
{
    while (pos < t.size() && t[pos] == ' ')
        ++pos;
    std::size_t const start = pos;
    while (pos < t.size() && t[pos] != ' ')
        ++pos;
    return t.substr(start, pos - start);
}

// Like ExternalEventHelper::ParseChatCommand: the whole text, then every leading run of words cut at a space. With
// everydayException, an EVERYDAY_TRIGGERS name counts only alone or before strategy syntax or '?'.
bool NamesTrigger(std::string_view t, TriggerSet const& triggers, std::size_t longestTrigger, bool everydayException)
{
    auto counts = [&](std::string_view name, std::size_t end)
    {
        if (triggers.find(name) == triggers.end())
            return false;
        if (!everydayException || !IsOneOf(name, EVERYDAY_TRIGGERS))
            return true;
        std::size_t next = end;
        while (next < t.size() && t[next] == ' ')
            ++next;
        if (next >= t.size())
            return true;
        char const c = t[next];
        return c == '+' || c == '-' || c == '~' || c == '?';
    };

    if (counts(t, t.size()))
        return true;

    for (std::size_t space = t.find(' '); space != std::string_view::npos && space <= longestTrigger;
         space = t.find(' ', space + 1))
        if (counts(t.substr(0, space), space))
            return true;

    return false;
}

struct Lexicon
{
    CoaIntent intent;
    char const* words;  // separated by spaces
    // Separated by commas. Each matches as consecutive folded words: anywhere, or with a leading '^' only as the
    // first words of the line.
    char const* phrases;
};

// In CoaIntent order: the first intent that matches wins. "nul", "trash", "combien" and "a plus" alone are too
// common in harmless lines ("c'est pas nul", "skip le trash", "combien de temps", "il n'y a plus de place").
Lexicon const LEXICON[] = {
    {CoaIntent::Insult,
     "noob nub n00b naze con conne connard connasse idiot idiote debile abruti abrutie cretin boulet tocard "
     "bouffon clown stfu stupid moron loser fdp tg",
     "ta gueule,ferme la,you suck,t es nul,t es nulle,tu es nul,tu es nulle,c est nul,you re trash,you are trash,"
     "ur trash"},
    {CoaIntent::Thanks, "merci mercii merciii thx thanks thank ty tyvm cimer", "thank you"},
    {CoaIntent::Lfg, "lf1m lf2m lf3m lf4m recrute",
     "qui pour,qui veut,cherche groupe,cherche un groupe,groupe pour,dispo pour,need tank,need heal,"
     "besoin d un tank,besoin d un heal"},
    {CoaIntent::Trade, "vends vend achete achat echange prix price sell buy", "how much,c est combien,ca coute"},
    {CoaIntent::Duel, "duel dueler 1v1 1c1", ""},
    {CoaIntent::Help, "aide aider aidez help boost escorte carry",
     "tu peux,vous pouvez,peux tu,pouvez vous,can you,could you,invite moi,inv moi,tp moi,besoin d aide"},
    {CoaIntent::AboutMe, "",
     "quelle classe,ta classe,quel niveau,ton niveau,quel lvl,ton lvl,ta spe,quelle spe,t es quoi,tu joues quoi,"
     "what class,what level,what spec,who are you,qui es tu,tu es qui"},
    {CoaIntent::Greeting, "salut slt bonjour bjr bonsoir coucou cc hello hi hey yo wesh hola", ""},
    {CoaIntent::Farewell, "bye byebye ciao tchao cya gn bn",
     "^a plus,a bientot,au revoir,bonne nuit,bonne soiree,see ya,see you,^a toute"},
    {CoaIntent::Compliment, "bravo gg gj wp classe genial super nice style",
     "bien joue,bien jouer,good job,well played,trop fort,trop forte,t es fort,t es forte"},
    {CoaIntent::Joke, "lol mdr ptdr xd rofl lmao jpp hihi", ""},
};

char const* const QUESTION_FIRST_WORDS =
    "comment pourquoi ou quand quel quelle quels quelles qui combien cb how why where when what which who anyone "
    "quelqu";

struct Phrase
{
    std::vector<std::string> words;
    bool anchored = false;  // matches only as the first words of the line
};

struct IntentTables
{
    std::unordered_map<std::string, uint32> wordIntents;  // bit i: the word belongs to CoaIntent i
    std::array<std::vector<Phrase>, std::size_t(CoaIntent::Count)> phrases;
    std::set<std::string, std::less<>> questionFirstWords;
};

std::vector<std::string> SplitOn(std::string_view text, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos)
            end = text.size();
        if (end > start)
            parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

IntentTables const& Tables()
{
    static IntentTables const tables = []
    {
        IntentTables built;
        for (Lexicon const& entry : LEXICON)
        {
            uint32 const bit = 1u << uint32(entry.intent);
            for (std::string const& word : SplitOn(entry.words, ' '))
                built.wordIntents[word] |= bit;
            // Phrases go through the same folding and splitting as the chat line.
            for (std::string const& phrase : SplitOn(entry.phrases, ','))
            {
                bool const anchored = phrase.front() == '^';
                std::string_view const text = std::string_view(phrase).substr(anchored ? 1 : 0);
                built.phrases[std::size_t(entry.intent)].push_back({NameMatch::Prepared(text).Words(), anchored});
            }
        }
        for (std::string const& word : SplitOn(QUESTION_FIRST_WORDS, ' '))
            built.questionFirstWords.insert(word);
        return built;
    }();
    return tables;
}

bool HasPhrase(std::vector<std::string> const& words, std::vector<Phrase> const& phrases)
{
    for (Phrase const& phrase : phrases)
    {
        if (phrase.words.empty() || phrase.words.size() > words.size())
            continue;
        std::size_t const lastStart = phrase.anchored ? 0 : words.size() - phrase.words.size();
        for (std::size_t start = 0; start <= lastStart; ++start)
            if (std::equal(phrase.words.begin(), phrase.words.end(), words.begin() + start))
                return true;
    }
    return false;
}

// "a+", "à+" and "++" as the whole line: a farewell with fewer than 2 letters.
bool IsShortFarewell(std::string_view text)
{
    std::string_view const trimmed = Trim(text);
    if (trimmed.size() > 3)  // "à+" is 3 bytes
        return false;
    std::string const whole = NameMatch::Fold(trimmed);
    return whole == "a+" || whole == "++";
}

// "haha", "hahaha", "hahah": (ha){2,}h?
bool IsLaugh(std::string_view word)
{
    if (word.size() % 2 == 1 && word.back() == 'h')
        word.remove_suffix(1);
    if (word.size() < 4 || word.size() % 2 == 1)
        return false;
    for (std::size_t i = 0; i < word.size(); i += 2)
        if (word[i] != 'h' || word[i + 1] != 'a')
            return false;
    return true;
}

char LastNonSpace(std::string_view text)
{
    std::string_view const trimmed = Trim(text);
    return trimmed.empty() ? '\0' : trimmed.back();
}
}  // namespace

bool CoaChatIntent::LooksLikeCommand(std::string_view text)
{
    std::string_view const trimmed = Trim(text);
    if (trimmed.empty())
        return false;

    // With a CommandPrefix, HandleCommand ignores every text without it, and the prefix is case-sensitive there.
    std::string const& commandPrefix = sPlayerbotAIConfig.commandPrefix;
    if (!commandPrefix.empty())
        return trimmed.substr(0, commandPrefix.size()) == commandPrefix;

    std::string const& commandSeparator = sPlayerbotAIConfig.commandSeparator;
    if (!commandSeparator.empty() && trimmed.find(commandSeparator) != std::string_view::npos)
        return true;

    std::string const lower = LowerAscii(trimmed);
    std::string_view const t = lower;

    for (std::string_view const prefix : CHAT_PREFIXES)
        if (t.substr(0, prefix.size()) == prefix)
            return true;
    if (t.front() == '@' || t.front() == '.' || t.front() == '!')
        return true;

    CommandVocabulary const& vocabulary = Commands();
    if (vocabulary.noReplyMsgs.find(t) != vocabulary.noReplyMsgs.end())
        return true;
    for (char const* part : NO_REPLY_MSG_PARTS)
        if (t.find(part) != std::string_view::npos)
            return true;
    for (std::string_view const start : NO_REPLY_MSG_STARTS)
        if (t.substr(0, start.size()) == start)
            return true;

    std::size_t pos = 0;
    std::string_view const first = WordAt(t, pos);
    if (IsStrategyToken(first))
        return true;
    if (IsOneOf(first, STRATEGY_OWNERS))
    {
        std::string_view const second = WordAt(t, pos);
        if (IsStrategyToken(second) || (second == "?" && pos >= t.size()))
            return true;
    }

    if (IsOneOf(t, INLINE_EXACT))
        return true;
    for (std::string_view const start : INLINE_STARTS)
        if (t.substr(0, start.size()) == start)
            return true;

    for (std::string const& command : PlayerbotAI::GetUnsecuredCommands())
        if (t.substr(0, command.size()) == command)
            return true;

    return NamesTrigger(t, vocabulary.triggers, vocabulary.longestTrigger, true);
}

bool CoaChatIntent::DispatchesAsTrigger(std::string_view text)
{
    // With a CommandPrefix, only prefixed text reaches ParseChatCommand, and LooksLikeCommand already takes it.
    if (!sPlayerbotAIConfig.commandPrefix.empty())
        return false;

    std::string_view const trimmed = Trim(text);
    if (trimmed.empty())
        return false;

    // HandleCommand queues the text as written (trimmed), so the names are compared case-sensitively here.
    CommandVocabulary const& vocabulary = Commands();
    return NamesTrigger(trimmed, vocabulary.exactTriggers, vocabulary.longestTrigger, false);
}

bool CoaChatIntent::HasLink(std::string_view text) { return text.find("|H") != std::string_view::npos; }

bool CoaChatIntent::IsConversation(std::string_view text)
{
    if (IsShortFarewell(text))
        return !LooksLikeCommand(text);  // only a CommandPrefix such as "+" could make it one
    if (CountLetters(text, 2) < 2 || HasLink(text))
        return false;

    // The first word, split as NameMatch::Prepared splits: the market words are ASCII, so folding is not needed.
    std::size_t start = 0;
    auto const inWord = [&](std::size_t i)
    {
        unsigned char const c = static_cast<unsigned char>(text[i]);
        return c >= 0x80 || IsAsciiLetter(c) || (c >= '0' && c <= '9');
    };
    while (start < text.size() && !inWord(start))
        ++start;
    std::size_t end = start;
    while (end < text.size() && inWord(end))
        ++end;
    if (end - start <= 3 && IsOneOf(LowerAscii(text.substr(start, end - start)), MARKET_WORDS))
        return false;

    return !LooksLikeCommand(text);
}

CoaIntent CoaChatIntent::Classify(std::string_view text)
{
    NameMatch::Prepared const prepared(text);
    std::vector<std::string> const& words = prepared.Words();
    IntentTables const& tables = Tables();

    uint32 wordMask = 0;
    for (std::string const& word : words)
    {
        auto const found = tables.wordIntents.find(word);
        if (found != tables.wordIntents.end())
            wordMask |= found->second;
    }

    for (Lexicon const& entry : LEXICON)
    {
        CoaIntent const intent = entry.intent;
        if ((wordMask & (1u << uint32(intent))) || HasPhrase(words, tables.phrases[std::size_t(intent)]))
            return intent;

        switch (intent)
        {
            case CoaIntent::Greeting:
                if (words.size() == 1 && words.front() == "re")
                    return intent;
                break;
            case CoaIntent::Farewell:
                if (IsShortFarewell(text))
                    return intent;
                break;
            case CoaIntent::Joke:
                for (std::string const& word : words)
                    if (IsLaugh(word))
                        return intent;
                break;
            default:
                break;
        }
    }

    if (LastNonSpace(text) == '?')
        return CoaIntent::Question;
    if (!words.empty() && tables.questionFirstWords.find(words.front()) != tables.questionFirstWords.end())
        return CoaIntent::Question;
    if (words.size() >= 2 && words[0] == "est" && words[1] == "ce")
        return CoaIntent::Question;

    return CountLetters(text, 1) ? CoaIntent::Other : CoaIntent::None;
}

char const* CoaChatIntent::IntentName(CoaIntent intent)
{
    static char const* const NAMES[] = {"none",     "insult",   "thanks",   "lfg",        "trade", "duel",     "help",
                                        "about-me", "greeting", "farewell", "compliment", "joke",  "question", "other"};
    static_assert(std::size(NAMES) == std::size_t(CoaIntent::Count), "one name per CoaIntent");
    std::size_t const index = std::size_t(intent);
    return index < std::size(NAMES) ? NAMES[index] : "none";
}
