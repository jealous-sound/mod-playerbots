/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINFEATURES_H
#define PLAYERBOTS_BRAINFEATURES_H

#include "CoaChatIntent.h"
#include "Define.h"
#include <string>
#include <string_view>
#include <vector>

enum class BrainLanguage : uint8
{
    Unknown = 0,
    French,
    English,
    Mixed
};

// What a hosted engine learns about a player's message when the words themselves are not shared
// (Privacy.SendPlayerText = 0): computed locally from the text, never containing any of it.
struct BrainMessageFeatures
{
    CoaIntent intent = CoaIntent::None;  // CoaChatIntent::Classify of the whole text
    std::vector<CoaIntent> also;         // Classify of each clause (split on . ! ? , ;), without `intent`, unique
    BrainLanguage language = BrainLanguage::Unknown;
    uint32 words = 0;          // NameMatch::Prepared words
    uint32 bytes = 0;
    bool question = false;     // ends with '?' or intent/also holds Question
    bool exclamation = false;  // holds '!'
    bool shouting = false;     // at least 4 ASCII letters and 70% of them upper-case
    bool laughter = false;     // intent or also holds Joke
    bool hasNumber = false;    // an ASCII digit
    uint32 namedBots = 0;      // set by the director: nominees whose name is in the text
};

// Pure functions; any thread.
namespace BrainFeatures
{
// Features of one chat line. Language: count of folded words in two small stopword lists (French: le la les
// un une des de du et est pas je tu il on nous vous ca c qui que quoi pour avec mais oui non merci salut
// bonjour; English: the a an and is are was not i you he we they it to of for with but yes no what how thanks
// hello); more of one (at least 1 and twice the other) -> that language; both at least 1 -> Mixed.
BrainMessageFeatures Extract(std::string_view text);

char const* LanguageName(BrainLanguage language);  // "unknown", "french", "english", "mixed"
char const* LengthBand(uint32 words);              // "short" (0-3 words), "medium" (4-12), "long" (13+)

// The key text of a short, common message for the cache: the folded words joined by one space when there are
// 1 to 3 of them and the result is at most 24 bytes ("salut", "gg", "merci beaucoup"); "" otherwise.
std::string CacheText(std::string_view text);
}  // namespace BrainFeatures

#endif
