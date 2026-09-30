/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SENTIMENTHEURISTIC_H
#define PLAYERBOTS_SENTIMENTHEURISTIC_H

#include "CoaChatIntent.h"
#include "Define.h"
#include "MemoryTypes.h"
#include <string_view>

struct CoaMemoryConfig;

// How a player's message or emote sounded, with no brain. Pure functions over read-only config lists: any thread
// (the voice director calls them on the world thread). The text is only read, never kept.
namespace SentimentHeuristic
{
// A message addressed to a bot. Whole folded words and phrases of config.friendlyWords / hostileWords (entries
// holding ASCII punctuation, such as "<3", ":D" or ":)", as raw substrings of the folded text), each entry counted
// once; the intent adds one: Thanks and Compliment friendly, Insult hostile. s = clamp(friendly - hostile, -2, 2)
// / 2, c = 0.6 (0 when nothing matched); insult = intent Insult; thanks = intent Thanks. Source Heuristic, or None
// when config.sentiment is 0 (then everything stays 0).
MemorySentiment Rate(std::string_view text, CoaIntent intent, CoaMemoryConfig const& config);

// A text emote aimed at a bot, by its CoaVoiceTemplates::EmoteCategoryFor category, c = 1: Thank +1 (thanks),
// Affection +1, Praise +1, Greet +0.5, Cheer +0.5, Apology +0.5, Bye +0.25, Laugh, Confused, Sad 0, Hurt -0.5,
// Gross -0.5, Rude -1 (insult). Source None when config.sentiment is 0.
MemorySentiment FromEmote(uint32 textEmote, CoaMemoryConfig const& config);
}  // namespace SentimentHeuristic

#endif
