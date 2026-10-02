/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SentimentHeuristic.h"
#include "CoaMemoryConfig.h"
#include "CoaVoiceTemplates.h"
#include "NameMatch.h"
#include <algorithm>
#include <string>
#include <vector>

namespace
{
constexpr float WORD_CONFIDENCE = 0.6f;

// An entry of CoaMemoryConfig's word lists: symbols (":)", "<3") are raw substrings of the folded text, the rest
// folded words separated by one space (see CoaMemoryConfig::Load).
bool IsSymbolEntry(std::string_view entry)
{
    for (char const c : entry)
    {
        unsigned char const u = static_cast<unsigned char>(c);
        if (u < 0x80 && u != ' ' && !((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')))
            return true;
    }
    return false;
}

// Whether the words hold the entry's words in a row.
bool HasPhrase(std::vector<std::string> const& words, std::string_view entry)
{
    std::vector<std::string_view> parts;
    for (std::size_t start = 0; start <= entry.size();)
    {
        std::size_t const end = std::min(entry.find(' ', start), entry.size());
        if (end > start)
            parts.push_back(entry.substr(start, end - start));
        start = end + 1;
    }
    if (parts.empty() || parts.size() > words.size())
        return false;

    for (std::size_t i = 0; i + parts.size() <= words.size(); ++i)
    {
        std::size_t matched = 0;
        while (matched < parts.size() && words[i + matched] == parts[matched])
            ++matched;
        if (matched == parts.size())
            return true;
    }
    return false;
}

uint32 CountMatches(std::vector<std::string> const& entries, std::vector<std::string> const& words,
                    std::string const& foldedText)
{
    uint32 matches = 0;
    for (std::string const& entry : entries)
    {
        bool const found = IsSymbolEntry(entry) ? foldedText.find(entry) != std::string::npos : HasPhrase(words, entry);
        if (found)
            ++matches;
    }
    return matches;
}
}  // namespace

MemorySentiment SentimentHeuristic::Rate(std::string_view text, CoaIntent intent, CoaMemoryConfig const& config)
{
    MemorySentiment sentiment;
    if (!config.sentiment)
        return sentiment;

    NameMatch::Prepared const prepared(text);
    std::string const folded = NameMatch::Fold(text);
    int32 friendly = int32(CountMatches(config.friendlyWords, prepared.Words(), folded));
    int32 hostile = int32(CountMatches(config.hostileWords, prepared.Words(), folded));
    if (intent == CoaIntent::Thanks || intent == CoaIntent::Compliment)
        ++friendly;
    else if (intent == CoaIntent::Insult)
        ++hostile;

    sentiment.s = float(std::clamp(friendly - hostile, -2, 2)) / 2.0f;
    sentiment.c = (friendly || hostile) ? WORD_CONFIDENCE : 0.0f;
    sentiment.insult = intent == CoaIntent::Insult;
    sentiment.thanks = intent == CoaIntent::Thanks;
    sentiment.source = MemorySentimentSource::Heuristic;
    return sentiment;
}

MemorySentiment SentimentHeuristic::FromEmote(uint32 textEmote, CoaMemoryConfig const& config)
{
    MemorySentiment sentiment;
    if (!config.sentiment)
        return sentiment;

    switch (CoaVoiceTemplates::EmoteCategoryFor(textEmote))
    {
        case CoaEmoteCategory::Thank:
            sentiment.s = 1.0f;
            sentiment.thanks = true;
            break;
        case CoaEmoteCategory::Affection:
        case CoaEmoteCategory::Praise:
            sentiment.s = 1.0f;
            break;
        case CoaEmoteCategory::Greet:
        case CoaEmoteCategory::Cheer:
        case CoaEmoteCategory::Apology:
            sentiment.s = 0.5f;
            break;
        case CoaEmoteCategory::Bye:
            sentiment.s = 0.25f;
            break;
        case CoaEmoteCategory::Hurt:
        case CoaEmoteCategory::Gross:
            sentiment.s = -0.5f;
            break;
        case CoaEmoteCategory::Rude:
            sentiment.s = -1.0f;
            sentiment.insult = true;
            break;
        default:  // Laugh, Confused, Sad, None
            sentiment.s = 0.0f;
            break;
    }
    sentiment.c = 1.0f;
    sentiment.source = MemorySentimentSource::Heuristic;
    return sentiment;
}
