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
    std::vector<BrainChatLine> lines;  // line option keys, in the order sent ("silent" is not listed here)
    uint32 whoCount = 0;               // options b0..b(whoCount-1) of the who question; 0 when not asked
};

// Builds what the brain is asked about one human message: the features-only state text and the questions. The
// pure functions work on plain facts (probes and the self-test use them without a Player); the others read
// Players and templates and run on the world thread only.
namespace BrainStateBuilder
{
// Pure. Sections in this fixed order, so the prefix is byte-stable: [realm] (the question set's header),
// [situation], [message], [bots]. At most maxChars bytes: bots lines are dropped from the end (b0 stays), then the
// message's "also" line; the header is never cut. Layout: see the contract, section "State text".
std::string ChatState(BrainChatFacts const& facts, BrainQuestionSet const& questions, uint32 maxChars);

// Pure. "level 23 Barbarian (Berserker), whisper receiver, named: no, same group: no, same guild: no, distance:
// near, fighting: no, answered in the last minute: no, chattiness: high".
std::string DescribeBot(BrainBotFacts const& bot);

// Pure. Questions in this order: reply (YesNo); who (Choice b0..bN-1, not cacheable) when askWho and more than one
// bot; line (Choice: each plan line as "[TAG] <NeutralTokens(row)>" with group = replyName, then "silent" with group
// "silent") when plan.lines is not empty; tone (Score, 5 levels); emote (Choice over QuestionRegistry::EmoteKeys()).
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
// bytes), questions (ChatQuestions), cacheKey (see the contract; "" unless BrainFeatures::CacheText is not empty).
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
}  // namespace BrainStateBuilder

#endif
