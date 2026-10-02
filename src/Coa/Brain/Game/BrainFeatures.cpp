/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainFeatures.h"
#include "NameMatch.h"
#include <algorithm>
#include <iterator>

namespace
{
// Function words of each language (folded), from the header's lists.
constexpr std::string_view FRENCH_WORDS[] = {
    "le", "la", "les", "un", "une", "des", "de", "du", "et", "est", "pas", "je", "tu", "il", "on",
    "nous", "vous", "ca", "c", "qui", "que", "quoi", "pour", "avec", "mais", "oui", "non", "merci", "salut", "bonjour"};
constexpr std::string_view ENGLISH_WORDS[] = {
    "the", "a", "an", "and", "is", "are", "was", "not", "i", "you", "he", "we", "they", "it", "to",
    "of", "for", "with", "but", "yes", "no", "what", "how", "thanks", "hello"};

template <std::size_t N>
bool In(std::string_view const (&list)[N], std::string_view word)
{
    return std::find(std::begin(list), std::end(list), word) != std::end(list);
}

bool Holds(BrainMessageFeatures const& features, CoaIntent intent)
{
    return features.intent == intent ||
           std::find(features.also.begin(), features.also.end(), intent) != features.also.end();
}

char LastNonSpace(std::string_view text)
{
    for (std::size_t i = text.size(); i > 0; --i)
    {
        char const c = text[i - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return c;
    }
    return 0;
}

bool IsClauseEnd(char c) { return c == '.' || c == '!' || c == '?' || c == ',' || c == ';'; }
}  // namespace

BrainMessageFeatures BrainFeatures::Extract(std::string_view text)
{
    BrainMessageFeatures features;
    features.bytes = uint32(text.size());
    features.intent = CoaChatIntent::Classify(text);

    // Each clause keeps its closing mark, so "salut, ca va ?" gives a greeting and then a question.
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = start;
        while (end < text.size() && !IsClauseEnd(text[end]))
            ++end;
        while (end < text.size() && IsClauseEnd(text[end]))
            ++end;

        std::string_view const clause = text.substr(start, end - start);
        start = end;

        CoaIntent const intent = CoaChatIntent::Classify(clause);
        // None and Other say nothing a model could use.
        if (intent == CoaIntent::None || intent == CoaIntent::Other || intent == features.intent)
            continue;
        if (std::find(features.also.begin(), features.also.end(), intent) == features.also.end())
            features.also.push_back(intent);
    }

    NameMatch::Prepared const prepared(text);
    std::vector<std::string> const& words = prepared.Words();
    features.words = uint32(words.size());

    uint32 french = 0;
    uint32 english = 0;
    for (std::string const& word : words)
    {
        if (In(FRENCH_WORDS, word))
            ++french;
        if (In(ENGLISH_WORDS, word))
            ++english;
    }
    if (french >= 1 && french >= 2 * english)
        features.language = BrainLanguage::French;
    else if (english >= 1 && english >= 2 * french)
        features.language = BrainLanguage::English;
    else if (french >= 1 && english >= 1)
        features.language = BrainLanguage::Mixed;

    uint32 letters = 0;
    uint32 upper = 0;
    for (char const c : text)
    {
        if (c >= 'A' && c <= 'Z')
        {
            ++letters;
            ++upper;
        }
        else if (c >= 'a' && c <= 'z')
            ++letters;
        else if (c >= '0' && c <= '9')
            features.hasNumber = true;
        else if (c == '!')
            features.exclamation = true;
    }
    features.shouting = letters >= 4 && upper * 10 >= letters * 7;
    features.question = LastNonSpace(text) == '?' || Holds(features, CoaIntent::Question);
    features.laughter = Holds(features, CoaIntent::Joke);
    return features;
}

char const* BrainFeatures::LanguageName(BrainLanguage language)
{
    switch (language)
    {
        case BrainLanguage::French:
            return "french";
        case BrainLanguage::English:
            return "english";
        case BrainLanguage::Mixed:
            return "mixed";
        default:
            return "unknown";
    }
}

char const* BrainFeatures::LengthBand(uint32 words)
{
    if (words <= 3)
        return "short";
    if (words <= 12)
        return "medium";
    return "long";
}

std::string BrainFeatures::CacheText(std::string_view text)
{
    NameMatch::Prepared const prepared(text);
    std::vector<std::string> const& words = prepared.Words();
    if (words.empty() || words.size() > 3)
        return "";

    std::string out;
    for (std::string const& word : words)
    {
        if (!out.empty())
            out += ' ';
        out += word;
    }
    return out.size() <= 24 ? out : "";
}
