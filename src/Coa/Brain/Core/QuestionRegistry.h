/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTIONREGISTRY_H
#define PLAYERBOTS_QUESTIONREGISTRY_H

#include "Define.h"
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// One canned chat situation for .playerbots brain probe: the message is run through the same state builder and
// question set as live chat, and the answer is compared with what a person expects.
struct BrainProbeCase
{
    std::string id;          // "fr-greet-1"
    std::string locale;      // "fr" or "en": probe reports each separately
    std::string scope;       // CoaVoiceScopeName: "whisper", "party", "say", "channel"...
    std::string message;     // what the player wrote (sent only to engines allowed player text)
    bool expectReply = true;
    std::string expectGroup; // the reply name a person would pick ("coa_reply_greet"), "" for none
};

// The English wording of the voice questions (Jev is strongest in English; the lines stay in the realm's
// language). Code owns every option key and what it does; this owns only the words. Immutable once published.
struct BrainQuestionSet
{
    std::string version;  // "voice-v1", or the file's own: part of every cache key, logged with each decision
    std::string source;   // "built-in" or the file's path
    std::string header;   // the [realm] section of every state

    std::string replyInstructions;
    std::string replyTrue;   // criteria "true"
    std::string replyFalse;  // criteria "false"

    std::string whoInstructions;

    std::string lineInstructions;
    std::string lineSilent;  // description of the "silent" option
    // M6: description of the "free_text" option, added to the line question only while the text lane applies to the
    // message (group "free_text" in the cache). Question file key: "line" -> "free_text".
    std::string lineFreeText;
    // Short reply name -> the English kind tag put in front of each line option ("greet" -> "GREETING"). Code
    // order: greet bye thanks question about_me help lfg trade insult compliment joke duel name generic.
    std::vector<std::pair<std::string, std::string>> lineTags;

    std::string toneInstructions;
    std::array<std::string, 5> toneLevels;  // 0 = hostile ... 4 = warm

    std::string emoteInstructions;
    // Emote key -> description. Code order and keys: none wave bow nod smile laugh cheer thank shrug flex.
    std::vector<std::pair<std::string, std::string>> emotes;

    std::vector<BrainProbeCase> probes;

    // M3 rating questions, asked with the voice decision only when AiPlayerbot.CoaMemory.Sentiment = 2 and
    // AiPlayerbot.CoaBrain.Use.Rating = 1. Question file keys: "friendliness" {"instructions", "levels"},
    // "insult" and "thanks" {"instructions", "criteria": {"true", "false"}}.
    std::string friendlinessInstructions;
    std::array<std::string, 5> friendlinessLevels;  // 0 = hostile ... 4 = warm
    std::string insultInstructions;
    std::string insultTrue;
    std::string insultFalse;
    std::string thanksInstructions;
    std::string thanksTrue;
    std::string thanksFalse;

    // The tag of a short reply name ("greet"), or the name itself upper-cased when the table has none.
    std::string LineTag(std::string_view shortName) const;
};

// Loads and publishes the question wording. The file (CoaBrain.QuestionFile, JSON, see the contract) overrides
// single strings of the compiled-in set; unknown keys and option keys the code does not know are ignored, a tone
// scale that is not exactly 5 levels is ignored, and a missing or broken file leaves the compiled-in set with one
// log line. The core installs only conf/*.conf.dist, so the compiled-in set is the real default.
namespace QuestionRegistry
{
// World thread (startup when the brain is on, .reload config, .playerbots brain on). "" = compiled-in.
void Reload(std::string const& file);

// Any thread. Never null: the compiled-in set before the first Reload.
std::shared_ptr<BrainQuestionSet const> Current();

// The compiled-in set ("voice-v1"), with its probe cases.
BrainQuestionSet BuiltIn();

// The emote keys, in code order ("none" first).
std::vector<std::string> const& EmoteKeys();
}  // namespace QuestionRegistry

#endif
