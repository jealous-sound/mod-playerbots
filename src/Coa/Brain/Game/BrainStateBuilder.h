/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINSTATEBUILDER_H
#define PLAYERBOTS_BRAINSTATEBUILDER_H

#include "BrainAdmin.h"
#include "BrainFeatures.h"
#include "BrainTypes.h"
#include "CoaChatIntent.h"
#include "CoaVoiceEvent.h"
#include "Define.h"
#include "MemoryTypes.h"
#include <string>
#include <string_view>
#include <vector>

class Player;
struct BrainQuestionSet;

// One bot as the brain sees it: game facts, never its name.
struct BrainBotFacts
{
    uint32 level = 0;
    std::string className;  // English class name (CoA classes included)
    std::string spec;       // specialization name, "" when unknown
    bool receiver = false;  // the whisper's receiver
    bool named = false;     // its name is in the message
    bool sameGroup = false;
    bool sameGuild = false;
    bool inCombat = false;
    bool answeredRecently = false;       // an answer reserved within the last 60 s
    char const* distance = "n/a";        // "near" (< 10 yd), "close" (< 25 yd), "far"; "n/a" outside say/yell/emote
    char const* chattiness = "medium";   // persona chattiness: "low" (< 0.45), "medium", "high" (> 0.75)
};

// One human message as the brain sees it.
struct BrainChatFacts
{
    CoaVoiceScopeKind kind = CoaVoiceScopeKind::Say;
    uint8 chainDepth = 0;           // 0: a real player spoke
    uint32 speakerLevel = 0;        // written as a band of 10 ("20-29")
    bool speakerGrouped = false;    // in a group with one of the bots
    bool speakerGuilded = false;    // in a guild with one of the bots
    BrainMessageFeatures features;  // namedBots set
    std::vector<BrainBotFacts> bots;  // ballot order, at most CoaVoice.Shortlist; b0 first
    // M3. askRating: ChatQuestions adds friendliness, insult and thanks (CoaMemoryConfig::UseBrainRating()).
    // memory: the [memory] lines (CoaMemoryMgr::BrainLines: features only, no names), already within
    // CoaMemory.StateTokenBudget; empty: no [memory] section.
    bool askRating = false;
    std::vector<std::string> memory;
};

// A line option of the line question.
struct BrainChatLine
{
    std::string key;        // "greet_1": short reply name + '_' + 1 or 2
    std::string replyName;  // "coa_reply_greet": its group (cache) and the text name the row came from
    std::string row;        // the row as stored: %player, %bot, %class, %spec, %level, %theirlevel, %zone unfilled
};

// What the director keeps to read the answers back.
struct BrainChatPlan
{
    std::vector<BrainChatLine> lines;  // line option keys, in the order sent ("silent" and "free_text" are not listed)
    uint32 whoCount = 0;               // options b0..b(whoCount-1) of the who question; 0 when not asked
    // M6: the line question also offers "free_text" (BrainQuestionSet::lineFreeText, group "free_text"): the text
    // lane applies to this message. The job's cache key gets "|t", so answers with and without the option never mix.
    bool freeText = false;
};

// M6: the line option key and cache group of "a generated line".
constexpr char const* BRAIN_FREE_TEXT_KEY = "free_text";

// Builds what the brain is asked about one human message: the features-only state text and the questions. The
// pure functions work on plain facts (probes and the self-test use them without a Player); the others read
// Players and templates and run on the world thread only.
namespace BrainStateBuilder
{
// Pure. Sections in this fixed order, so the prefix is byte-stable: [realm] (the question set's header),
// [situation], [message], [bots], then [memory] when facts.memory is not empty. At most maxChars bytes: memory lines
// are dropped from the end first, then bots lines from the end (b0 stays), then the message's "also" line; the
// header is never cut. Layout: see the M2 contract, section "State text", and the M3 contract for [memory].
std::string ChatState(BrainChatFacts const& facts, BrainQuestionSet const& questions, uint32 maxChars);

// Pure. "level 23 Barbarian (Berserker), whisper receiver, named: no, same group: no, same guild: no, distance:
// near, fighting: no, answered in the last minute: no, chattiness: high".
std::string DescribeBot(BrainBotFacts const& bot);

// Pure. Questions in this order: reply (YesNo); who (Choice b0..bN-1, not cacheable) when askWho and more than one
// bot; line (Choice: each plan line as "[TAG] <NeutralTokens(row)>" with group = replyName, then, when plan.freeText,
// "free_text" (BrainQuestionSet::lineFreeText, group "free_text"), then "silent" with group "silent") when plan.lines
// is not empty or plan.freeText; tone (Score, 5 levels); emote (Choice over QuestionRegistry::EmoteKeys());
// then, when facts.askRating (M3): friendliness (Score, 5 levels), insult (YesNo), thanks (YesNo), all cacheable.
std::vector<BrainQuestion> ChatQuestions(BrainChatFacts const& facts, BrainChatPlan const& plan,
                                         BrainQuestionSet const& questions, bool askWho);

// Pure. The tokens of a row as the brain sees them: %player -> {name}, %bot -> {me}, %class -> {class}, %spec ->
// {spec}, %level -> {level}, %theirlevel -> {their_level}, %zone -> {zone}, %s -> {name}. The names are filled in
// only after the decision, by the director, for the bot that answers.
std::string NeutralTokens(std::string row);

// World thread. The facts of one bot answering `speaker` in a scope of that kind.
BrainBotFacts BotFacts(Player* bot, Player* speaker, CoaVoiceScopeKind kind, bool receiver, bool named,
                       bool answeredRecently);

// World thread (templates). Up to 8 line options: 2 rows (CoaVoiceTemplates::Candidates) for each of up to 4
// reply names around `intent` (the table in the contract), skipping names without rows. Keys "<short>_<n>".
std::vector<BrainChatLine> ChatLines(CoaIntent intent, bool named, uint32 locale);

// World thread. The whole chat_reply job: use ChatReply, lane Player, situation "chat_reply", state (ChatState
// with CoaBrain.StateMaxChars), playerText (the message: control characters and "<<<", ">>>" removed, cut to 255
// bytes), questions (ChatQuestions), cacheKey (see the contract; "" unless BrainFeatures::CacheText is not empty;
// M3: "|r" appended with askRating, "|m<16 hex digits of CoaHash::Bytes of the joined memory lines>" with memory;
// M6: "|t" appended with plan.freeText).
// The caller sets tag, epoch, botGuid and deadlineMs. False when there is nothing to ask.
bool BuildChatJob(BrainChatFacts const& facts, std::string_view text, BrainChatPlan const& plan, bool askWho,
                  BrainJob& job);

// World thread (.playerbots brain test <bot> <message>). A whisper from `speaker` (the GM; null from the console:
// a level-60 player is assumed) to `bot`, as the director would ask it; `out.lines` holds the rows filled for the
// bot. False, with `error`, when the bot is not an online bot.
bool BuildTestJob(Player* bot, Player* speaker, std::string const& message, BrainTestJob& out, std::string& error);

// World thread (.playerbots brain probe). One job per probe case of the question set: facts from the case (scope,
// message features, one level-30 bot, speaker level 30), lines from ChatLines of the message's intent.
std::vector<BrainProbeJob> BuildProbeJobs(BrainQuestionSet const& questions, uint32 locale);

// The game value of a code-owned emote key: "wave" -> TEXT_EMOTE_WAVE ... ; 0 for "none" and unknown keys.
uint32 EmoteFor(std::string_view key);

// "hostile", "cold", "neutral", "friendly", "warm" for 0..4; "unknown" otherwise.
char const* ToneName(int32 level);

// Pure (M3). The rating answers of a chat_reply result over `sentiment`, which holds the heuristic: a valid
// friendliness sets s = (level - 2) / 2 and c = its p when calibrated, else 0.3; a valid insult or thanks sets that
// flag to (p >= 0.7). True, with source Brain, when at least one of the three was valid; false leaves `sentiment`
// untouched.
bool ReadRating(BrainResult const& result, MemorySentiment& sentiment);
}  // namespace BrainStateBuilder

#endif
