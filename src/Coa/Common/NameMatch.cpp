/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NameMatch.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace
{
// U+00C0..U+00FF folded; null keeps the character (multiplication and division signs).
char const* const LATIN1_FOLD[64] = {
    "a", "a", "a", "a", "a", "a", "ae", "c",     "e", "e", "e", "e", "i", "i", "i",  "i",   // U+00C0
    "d", "n", "o", "o", "o", "o", "o",  nullptr, "o", "u", "u", "u", "u", "y", "th", "ss",  // U+00D0
    "a", "a", "a", "a", "a", "a", "ae", "c",     "e", "e", "e", "e", "i", "i", "i",  "i",   // U+00E0
    "d", "n", "o", "o", "o", "o", "o",  nullptr, "o", "u", "u", "u", "u", "y", "th", "y",   // U+00F0
};

bool IsContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// Length of the valid UTF-8 sequence starting at text[i] (1 to 4), or 0 when the byte there starts none.
// Overlong forms, surrogates and code points above U+10FFFF are invalid.
std::size_t SequenceLength(std::string_view text, std::size_t i, uint32_t& codePoint)
{
    unsigned char const lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80)
    {
        codePoint = lead;
        return 1;
    }

    std::size_t length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF)
    {
        length = 2;
        codePoint = lead & 0x1F;
    }
    else if (lead >= 0xE0 && lead <= 0xEF)
    {
        length = 3;
        codePoint = lead & 0x0F;
        if (lead == 0xE0)
            low = 0xA0;
        else if (lead == 0xED)
            high = 0x9F;
    }
    else if (lead >= 0xF0 && lead <= 0xF4)
    {
        length = 4;
        codePoint = lead & 0x07;
        if (lead == 0xF0)
            low = 0x90;
        else if (lead == 0xF4)
            high = 0x8F;
    }
    else
        return 0;

    if (i + length > text.size())
        return 0;

    for (std::size_t k = 1; k < length; ++k)
    {
        unsigned char const c = static_cast<unsigned char>(text[i + k]);
        if (k == 1 ? (c < low || c > high) : !IsContinuation(c))
            return 0;
        codePoint = (codePoint << 6) | (c & 0x3F);
    }

    return length;
}

bool IsAsciiAlnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool IsAsciiLetter(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Folded words that never name anyone and never open a line as a vocative: French and English function words,
// greetings and MMO jargon. Several are random-bot names (Bon, Mais, Des, Elle, Juste, Allez...), and more are the
// start of one ("elle" of Elleaan, "more" of Moreadon): read as names, they forced answers or silenced every bot.
constexpr std::string_view STOPWORDS[] = {
    "about", "add", "adds", "afk", "aggro", "all", "allez", "alors", "and", "any", "anyone", "apres", "are", "aussi",
    "autre", "avant", "avec", "bah", "ben", "bien", "bon", "bonjour", "bonsoir", "boss", "brb", "bref", "buff",
    "but", "can", "car", "ces", "cette", "chez", "come", "comme", "comment", "coucou", "could", "dans", "deja",
    "des", "did", "does", "donc", "donjon", "dont", "dps", "dungeon", "elle", "elles", "encore", "est", "etait",
    "fait", "faut", "for", "from", "get", "gold", "got", "groupe", "grp", "guild", "guilde", "guys", "has", "have",
    "heal", "healer", "hello", "here", "hey", "hi", "how", "ici", "ils", "inv", "just", "juste", "kill", "know",
    "les", "lets", "leur", "level", "like", "lol", "loot", "lui", "lvl", "mais", "mdr", "merci", "mob", "mobs",
    "moi", "mon", "more", "mort", "much", "need", "no", "non", "not", "nous", "now", "okay", "one", "only", "ouais",
    "oui", "party", "pas", "peu", "peut", "please", "pls", "plus", "plz", "pour", "pourquoi", "pull", "quand", "que",
    "quel", "quelle", "quest", "quete", "qui", "quoi", "raid", "ready", "rez", "rien", "salut", "sans", "ses",
    "some", "son", "sont", "sorry", "still", "stp", "stuff", "sur", "sure", "svp", "tank", "than", "thank", "thanks",
    "that", "the", "them", "then", "there", "they", "this", "toi", "ton", "too", "tous", "tout", "toute", "toutes",
    "tres", "trop", "une", "vais", "vas", "vers", "very", "veux", "viens", "voila", "vous", "want", "was", "well",
    "were", "what", "when", "where", "which", "who", "why", "will", "wipe", "with", "would", "yeah", "yep", "yes",
    "you", "your", "zone"};
static_assert(std::is_sorted(std::begin(STOPWORDS), std::end(STOPWORDS)), "STOPWORDS is searched by bisection");

bool IsStopword(std::string_view folded)
{
    return std::binary_search(std::begin(STOPWORDS), std::end(STOPWORDS), folded);
}
}  // namespace

std::string NameMatch::Fold(std::string_view utf8)
{
    std::string folded;
    folded.reserve(utf8.size());

    std::size_t i = 0;
    while (i < utf8.size())
    {
        uint32_t codePoint = 0;
        std::size_t const length = SequenceLength(utf8, i, codePoint);
        if (!length)
        {
            ++i;  // invalid byte: dropped, decoding resumes at the next one
            continue;
        }

        if (length == 1)
        {
            char c = utf8[i];
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
            folded += c;
        }
        else if (codePoint >= 0xC0 && codePoint <= 0xFF && LATIN1_FOLD[codePoint - 0xC0])
            folded += LATIN1_FOLD[codePoint - 0xC0];
        else if (codePoint == 0x152 || codePoint == 0x153)
            folded += "oe";
        else if (codePoint == 0x2019)
            folded += '\'';
        else
            folded.append(utf8.data() + i, length);

        i += length;
    }

    return folded;
}

NameMatch::Prepared::Prepared(std::string_view text)
{
    std::string const folded = Fold(text);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= folded.size(); ++i)
    {
        // Bytes of multi-byte characters (0x80 and above) count as letters; every other non-alphanumeric
        // ASCII byte splits, apostrophes and hyphens included.
        bool const inWord = i < folded.size() && (static_cast<unsigned char>(folded[i]) >= 0x80 ||
                                                  IsAsciiAlnum(static_cast<unsigned char>(folded[i])));
        if (inWord)
            continue;
        if (i > start)
        {
            words.emplace_back(folded, start, i - start);
            stopwords.push_back(IsStopword(words.back()));
        }
        start = i + 1;
    }

    std::size_t first = 0;
    while (first < text.size() && text[first] == ' ')
        ++first;
    std::size_t end = first;
    while (end < text.size() &&
           (static_cast<unsigned char>(text[end]) >= 0x80 || IsAsciiLetter(static_cast<unsigned char>(text[end]))))
        ++end;
    std::string_view const opener = text.substr(first, end - first);
    if (opener.size() >= 3 && (end == text.size() || text[end] == ',' || text[end] == ':' || text[end] == ' ') &&
        !IsStopword(Fold(opener)))
        vocative.assign(opener);
}

bool NameMatch::Prepared::Mentions(std::string_view foldedName) const
{
    if (foldedName.size() < 3)
        return false;

    for (std::size_t i = 0; i < words.size(); ++i)
    {
        if (stopwords[i])
            continue;
        std::string const& word = words[i];
        if (word == foldedName)
            return true;
        if (foldedName.size() >= 6 && word.size() >= 4 && word.size() < foldedName.size() &&
            foldedName.compare(0, word.size(), word) == 0)
            return true;
    }

    return false;
}

bool NameMatch::Mentions(std::string_view text, std::string_view name) { return Prepared(text).Mentions(Fold(name)); }
