/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainStateBuilder.h"
#include "BrainConfig.h"
#include "CoaHash.h"
#include "CoaSpecLookup.h"
#include "CoaSpecialization.h"
#include "CoaText.h"
#include "CoaThread.h"
#include "CoaVoiceConfig.h"
#include "CoaVoicePersona.h"
#include "CoaVoiceTemplates.h"
#include "DBCStores.h"
#include "Group.h"
#include "NameMatch.h"
#include "Player.h"
#include "Playerbots.h"
#include "QuestionRegistry.h"
#include "SharedDefines.h"
#include <algorithm>
#include <array>

namespace
{
constexpr std::string_view REPLY_PREFIX = "coa_reply_";
constexpr std::size_t MAX_LINE_NAMES = 4;
constexpr uint32 ROWS_PER_NAME = 2;
constexpr std::size_t PLAYER_TEXT_MAX_BYTES = 255;

char const* YesNo(bool value) { return value ? "yes" : "no"; }

std::string_view FirstName(std::string const& name)
{
    std::string_view const view(name);
    return view.substr(0, view.find(' '));
}

// The director's ClassName: the CoA class name, else the English WotLK one.
std::string ClassName(Player* bot)
{
    if (char const* coa = CoaClassName(bot->getClass()))
        return coa;

    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
            return "Warrior";
        case CLASS_PALADIN:
            return "Paladin";
        case CLASS_HUNTER:
            return "Hunter";
        case CLASS_ROGUE:
            return "Rogue";
        case CLASS_PRIEST:
            return "Priest";
        case CLASS_DEATH_KNIGHT:
            return "Death Knight";
        case CLASS_SHAMAN:
            return "Shaman";
        case CLASS_MAGE:
            return "Mage";
        case CLASS_WARLOCK:
            return "Warlock";
        case CLASS_DRUID:
            return "Druid";
        default:
            return "";
    }
}

char const* Place(CoaVoiceScopeKind kind)
{
    switch (kind)
    {
        case CoaVoiceScopeKind::Whisper:
            return "a whisper to one bot (b0)";
        case CoaVoiceScopeKind::Party:
            return "party chat";
        case CoaVoiceScopeKind::Raid:
            return "raid chat";
        case CoaVoiceScopeKind::Guild:
            return "guild chat";
        case CoaVoiceScopeKind::Say:
            return "say, heard by the bots nearby";
        case CoaVoiceScopeKind::Yell:
            return "yell, heard across the area";
        case CoaVoiceScopeKind::CustomEmote:
            return "an emote (/e) naming a bot";
        case CoaVoiceScopeKind::TextEmote:
            return "a text emote at a bot";
        case CoaVoiceScopeKind::Channel:
            return "a public zone channel";
        default:
            return "unknown";
    }
}

std::string LevelBand(uint32 level)
{
    if (!level)
        return "unknown";
    if (level < 10)
        return "1-9";
    if (level >= 80)
        return "80";
    uint32 const low = level / 10 * 10;
    return std::to_string(low) + "-" + std::to_string(low + 9);
}

bool IsLocal(CoaVoiceScopeKind kind)
{
    return kind == CoaVoiceScopeKind::Say || kind == CoaVoiceScopeKind::Yell ||
           kind == CoaVoiceScopeKind::CustomEmote || kind == CoaVoiceScopeKind::TextEmote;
}

CoaVoiceScopeKind KindFromName(std::string_view name)
{
    for (uint8 kind = 0; kind < uint8(CoaVoiceScopeKind::Count); ++kind)
        if (name == CoaVoiceScopeName(CoaVoiceScopeKind(kind)))
            return CoaVoiceScopeKind(kind);
    return CoaVoiceScopeKind::Say;
}

// The reply names asked about after the primary one (short names), per intent: the contract's table.
std::array<char const*, 3> RelatedReplies(CoaIntent intent)
{
    switch (intent)
    {
        case CoaIntent::Greeting:
            return {"question", "joke", "generic"};
        case CoaIntent::Farewell:
            return {"thanks", "joke", "generic"};
        case CoaIntent::Thanks:
            return {"compliment", "greet", "generic"};
        case CoaIntent::Question:
            return {"about_me", "help", "generic"};
        case CoaIntent::AboutMe:
            return {"question", "greet", "generic"};
        case CoaIntent::Help:
            return {"lfg", "question", "generic"};
        case CoaIntent::Lfg:
            return {"help", "question", "generic"};
        case CoaIntent::Trade:
            return {"question", "joke", "generic"};
        case CoaIntent::Insult:
            return {"joke", "bye", "generic"};
        case CoaIntent::Compliment:
            return {"thanks", "joke", "generic"};
        case CoaIntent::Joke:
            return {"compliment", "greet", "generic"};
        case CoaIntent::Duel:
            return {"insult", "joke", "generic"};
        default:
            return {"question", "greet", "joke"};
    }
}

std::string ShortReplyName(std::string_view replyName)
{
    if (replyName.starts_with(REPLY_PREFIX))
        replyName.remove_prefix(REPLY_PREFIX.size());
    return std::string(replyName);
}

// Control bytes and the markers SystemOneWire puts around the words removed, so the text cannot close its own
// section; then cut to the core's chat cap.
std::string PlayerText(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
        if (static_cast<unsigned char>(c) >= 0x20)
            out += c;

    for (bool again = true; again;)
    {
        again = false;
        for (char const* marker : {"<<<", ">>>"})
        {
            for (std::size_t at = out.find(marker); at != std::string::npos; at = out.find(marker, at))
            {
                out.erase(at, 3);
                again = true;
            }
        }
    }

    CoaText::CutUtf8Bytes(out, PLAYER_TEXT_MAX_BYTES);
    return out;
}

std::size_t JoinedSize(std::vector<std::string> const& lines)
{
    std::size_t size = lines.empty() ? 0 : lines.size() - 1;
    for (std::string const& line : lines)
        size += line.size();
    return size;
}

// 16 lower-case hex digits.
std::string Hex16(uint64 value)
{
    static constexpr char DIGITS[] = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t i = out.size(); i-- > 0; value >>= 4)
        out[i] = DIGITS[value & 0xF];
    return out;
}

// M3: the memory lines of the state, as one string for the cache key.
std::string JoinLines(std::vector<std::string> const& lines)
{
    std::string out;
    out.reserve(JoinedSize(lines));
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i)
            out += '\n';
        out += lines[i];
    }
    return out;
}

// The line placeholders filled for `bot` answering `speaker` (the console: a level-60 "Player").
CoaVoiceTemplates::Placeholders TestPlaceholders(Player* bot, Player* speaker)
{
    using namespace CoaVoicePlaceholder;
    std::string const className = ClassName(bot);
    CoaSpecStrategy const* const spec = GetCoaSpecStrategyFor(bot);

    CoaVoiceTemplates::Placeholders placeholders;
    placeholders.Set(PLAYER, speaker ? std::string(FirstName(speaker->GetName())) : std::string("Player"))
        .Set(BOT, std::string(FirstName(bot->GetName())))
        .Set(CLASS, className)
        .Set(SPEC, spec && spec->specName ? std::string(spec->specName) : className)
        .Set(LEVEL, std::to_string(bot->GetLevel()))
        .Set(THEIR_LEVEL, std::to_string(speaker ? speaker->GetLevel() : 60))
        .Set(ZONE, PlayerbotAI::GetLocalizedAreaName(sAreaTableStore.LookupEntry(bot->GetZoneId())));
    return placeholders;
}

// The job every builder shares: questions from `questions`, the state bounded by maxChars.
bool FillChatJob(BrainChatFacts const& facts, std::string_view text, BrainChatPlan const& plan, bool askWho,
                 BrainQuestionSet const& questions, uint32 maxChars, BrainJob& job)
{
    if (facts.bots.empty())
        return false;

    job.use = BrainUse::ChatReply;
    job.lane = BrainLane::Player;
    job.situation = "chat_reply";
    job.state = BrainStateBuilder::ChatState(facts, questions, maxChars);
    job.playerText = PlayerText(text);
    job.questions = BrainStateBuilder::ChatQuestions(facts, plan, questions, askWho);
    if (job.questions.empty())
        return false;

    job.cacheKey.clear();
    std::string const cacheText = BrainFeatures::CacheText(text);
    if (!cacheText.empty())
    {
        job.cacheKey = "chat_reply|" + questions.version + "|" + CoaVoiceScopeName(facts.kind) + "|" +
                       CoaChatIntent::IntentName(facts.features.intent) + "|" +
                       (facts.features.namedBots ? "named" : "-") + "|" +
                       std::to_string(std::min<std::size_t>(facts.bots.size(), 3)) + "|" + cacheText;
        if (plan.freeText)
            job.cacheKey += "|t";
        // M3: rated answers and answers given with memory lines are never served for another kind of job.
        if (facts.askRating)
            job.cacheKey += "|r";
        if (!facts.memory.empty())
            job.cacheKey += "|m" + Hex16(CoaHash::Bytes(JoinLines(facts.memory)));
    }
    return true;
}
}  // namespace

std::string BrainStateBuilder::ChatState(BrainChatFacts const& facts, BrainQuestionSet const& questions,
                                         uint32 maxChars)
{
    BrainMessageFeatures const& features = facts.features;
    std::vector<std::string> lines;
    lines.reserve(20 + facts.bots.size());

    lines.emplace_back("[realm]");
    lines.push_back(questions.header);
    std::size_t const headerLines = lines.size();

    lines.emplace_back("[situation]");
    lines.push_back(std::string("place: ") + Place(facts.kind));
    std::string speaker = "speaker: a real player, level " + LevelBand(facts.speakerLevel);
    if (facts.speakerGrouped)
        speaker += ", grouped with a bot";
    if (facts.speakerGuilded)
        speaker += ", in a guild with a bot";
    lines.push_back(std::move(speaker));

    lines.emplace_back("[message]");
    lines.push_back(std::string("language: ") + BrainFeatures::LanguageName(features.language));
    lines.push_back(std::string("intent: ") + CoaChatIntent::IntentName(features.intent));
    std::string also = "also: ";
    for (std::size_t i = 0; i < features.also.size(); ++i)
        also += std::string(i ? ", " : "") + CoaChatIntent::IntentName(features.also[i]);
    if (features.also.empty())
        also += "none";
    std::size_t const alsoLine = lines.size();
    lines.push_back(std::move(also));
    lines.push_back(std::string("length: ") + BrainFeatures::LengthBand(features.words) + " (" +
                    std::to_string(features.words) + " words)");
    lines.push_back(std::string("question: ") + YesNo(features.question));
    lines.push_back(std::string("exclamation: ") + YesNo(features.exclamation));
    lines.push_back(std::string("shouting: ") + YesNo(features.shouting));
    lines.push_back(std::string("laughter: ") + YesNo(features.laughter));
    lines.push_back(std::string("number: ") + YesNo(features.hasNumber));
    lines.push_back(std::string("names a bot: ") +
                    (features.namedBots == 0 ? "no" : features.namedBots == 1 ? "yes" : "several"));

    std::size_t firstBot = lines.size();
    if (!facts.bots.empty())
    {
        lines.emplace_back("[bots]");
        firstBot = lines.size();
        for (std::size_t i = 0; i < facts.bots.size(); ++i)
            lines.push_back("b" + std::to_string(i) + ": " + DescribeBot(facts.bots[i]));
    }

    // M3: what the bots remember about the speaker goes first when the state is too long; with its last line the
    // section header goes too.
    if (!facts.memory.empty())
    {
        std::size_t const memoryHeader = lines.size();
        lines.emplace_back("[memory]");
        lines.insert(lines.end(), facts.memory.begin(), facts.memory.end());
        while (JoinedSize(lines) > maxChars && lines.size() > memoryHeader + 1)
            lines.pop_back();
        if (lines.size() == memoryHeader + 1)
            lines.pop_back();
    }

    // Over the limit: the last bots first (b0 stays), then the "also" line. The header is never cut.
    while (JoinedSize(lines) > maxChars && lines.size() > firstBot + 1)
        lines.pop_back();
    if (JoinedSize(lines) > maxChars)
        lines.erase(lines.begin() + std::ptrdiff_t(alsoLine));
    // Only a StateMaxChars smaller than the whole minimal state gets here: whole lines go from the end.
    while (JoinedSize(lines) > maxChars && lines.size() > headerLines)
        lines.pop_back();

    std::string out;
    out.reserve(JoinedSize(lines));
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i)
            out += '\n';
        out += lines[i];
    }
    return out;
}

std::string BrainStateBuilder::DescribeBot(BrainBotFacts const& bot)
{
    std::string out = "level " + std::to_string(bot.level);
    if (!bot.className.empty())
        out += " " + bot.className;
    if (!bot.spec.empty())
        out += " (" + bot.spec + ")";
    if (bot.receiver)
        out += ", whisper receiver";
    out += std::string(", named: ") + YesNo(bot.named);
    out += std::string(", same group: ") + YesNo(bot.sameGroup);
    out += std::string(", same guild: ") + YesNo(bot.sameGuild);
    out += std::string(", distance: ") + (bot.distance ? bot.distance : "n/a");
    out += std::string(", fighting: ") + YesNo(bot.inCombat);
    out += std::string(", answered in the last minute: ") + YesNo(bot.answeredRecently);
    out += std::string(", chattiness: ") + (bot.chattiness ? bot.chattiness : "medium");
    return out;
}

std::vector<BrainQuestion> BrainStateBuilder::ChatQuestions(BrainChatFacts const& facts, BrainChatPlan const& plan,
                                                            BrainQuestionSet const& questions, bool askWho)
{
    std::vector<BrainQuestion> out;
    out.reserve(facts.askRating ? 8 : 5);

    BrainQuestion reply;
    reply.name = "reply";
    reply.type = BrainQuestionType::YesNo;
    reply.instructions = questions.replyInstructions;
    reply.options = {{"true", questions.replyTrue, ""}, {"false", questions.replyFalse, ""}};
    out.push_back(std::move(reply));

    if (askWho && facts.bots.size() > 1)
    {
        BrainQuestion who;
        who.name = "who";
        who.type = BrainQuestionType::Choice;
        who.instructions = questions.whoInstructions;
        who.cacheable = false;
        for (std::size_t i = 0; i < facts.bots.size(); ++i)
            who.options.push_back({"b" + std::to_string(i), DescribeBot(facts.bots[i]), ""});
        out.push_back(std::move(who));
    }

    if (!plan.lines.empty() || plan.freeText)
    {
        BrainQuestion line;
        line.name = "line";
        line.type = BrainQuestionType::Choice;
        line.instructions = questions.lineInstructions;
        for (BrainChatLine const& candidate : plan.lines)
        {
            std::string text = "[" + questions.LineTag(ShortReplyName(candidate.replyName)) + "] " +
                               NeutralTokens(candidate.row);
            line.options.push_back({candidate.key, std::move(text), candidate.replyName});
        }
        if (plan.freeText)
            line.options.push_back({BRAIN_FREE_TEXT_KEY, questions.lineFreeText, BRAIN_FREE_TEXT_KEY});
        line.options.push_back({"silent", questions.lineSilent, "silent"});
        out.push_back(std::move(line));
    }

    BrainQuestion tone;
    tone.name = "tone";
    tone.type = BrainQuestionType::Score;
    tone.instructions = questions.toneInstructions;
    for (std::size_t level = 0; level < questions.toneLevels.size(); ++level)
        tone.options.push_back({std::to_string(level), questions.toneLevels[level], ""});
    out.push_back(std::move(tone));

    BrainQuestion emote;
    emote.name = "emote";
    emote.type = BrainQuestionType::Choice;
    emote.instructions = questions.emoteInstructions;
    for (std::string const& key : QuestionRegistry::EmoteKeys())
    {
        auto const described = std::find_if(questions.emotes.begin(), questions.emotes.end(),
                                            [&key](auto const& entry) { return entry.first == key; });
        emote.options.push_back({key, described != questions.emotes.end() ? described->second : key, ""});
    }
    out.push_back(std::move(emote));

    // M3: the bots' memory rates the message (ReadRating reads these answers).
    if (facts.askRating)
    {
        BrainQuestion friendliness;
        friendliness.name = "friendliness";
        friendliness.type = BrainQuestionType::Score;
        friendliness.instructions = questions.friendlinessInstructions;
        for (std::size_t level = 0; level < questions.friendlinessLevels.size(); ++level)
            friendliness.options.push_back({std::to_string(level), questions.friendlinessLevels[level], ""});
        out.push_back(std::move(friendliness));

        BrainQuestion insult;
        insult.name = "insult";
        insult.type = BrainQuestionType::YesNo;
        insult.instructions = questions.insultInstructions;
        insult.options = {{"true", questions.insultTrue, ""}, {"false", questions.insultFalse, ""}};
        out.push_back(std::move(insult));

        BrainQuestion thanks;
        thanks.name = "thanks";
        thanks.type = BrainQuestionType::YesNo;
        thanks.instructions = questions.thanksInstructions;
        thanks.options = {{"true", questions.thanksTrue, ""}, {"false", questions.thanksFalse, ""}};
        out.push_back(std::move(thanks));
    }
    return out;
}

std::string BrainStateBuilder::NeutralTokens(std::string row)
{
    // Longest first: "%s" is the start of "%spec".
    static constexpr std::pair<std::string_view, std::string_view> TOKENS[] = {
        {"%theirlevel", "{their_level}"}, {"%player", "{name}"}, {"%class", "{class}"}, {"%level", "{level}"},
        {"%spec", "{spec}"},              {"%zone", "{zone}"},   {"%bot", "{me}"},      {"%s", "{name}"}};

    std::string out;
    out.reserve(row.size() + 16);
    std::string_view const view(row);
    for (std::size_t i = 0; i < view.size();)
    {
        bool replaced = false;
        if (view[i] == '%')
        {
            for (auto const& [token, neutral] : TOKENS)
            {
                if (view.substr(i).starts_with(token))
                {
                    out += neutral;
                    i += token.size();
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced)
            out += view[i++];
    }
    return out;
}

BrainBotFacts BrainStateBuilder::BotFacts(Player* bot, Player* speaker, CoaVoiceScopeKind kind, bool receiver,
                                          bool named, bool answeredRecently)
{
    COA_ASSERT_WORLD_THREAD();

    BrainBotFacts facts;
    facts.level = bot->GetLevel();
    facts.className = ClassName(bot);
    if (CoaSpecStrategy const* const spec = GetCoaSpecStrategyFor(bot); spec && spec->specName)
        facts.spec = spec->specName;
    facts.receiver = receiver;
    facts.named = named;
    facts.answeredRecently = answeredRecently;
    facts.inCombat = bot->IsInCombat();

    if (speaker)
    {
        facts.sameGroup = bot->GetGroup() && bot->GetGroup() == speaker->GetGroup();
        facts.sameGuild = bot->GetGuildId() && bot->GetGuildId() == speaker->GetGuildId();
    }

    if (IsLocal(kind) && speaker)
    {
        if (bot->GetMapId() != speaker->GetMapId() || bot->GetInstanceId() != speaker->GetInstanceId())
            facts.distance = "far";
        else
        {
            float const distance = bot->GetExactDist(speaker);
            facts.distance = distance < 10.0f ? "near" : distance < 25.0f ? "close" : "far";
        }
    }

    float const chattiness = CoaVoicePersona::For(bot->GetGUID()).chattiness;
    facts.chattiness = chattiness < 0.45f ? "low" : chattiness > 0.75f ? "high" : "medium";
    return facts;
}

std::vector<BrainChatLine> BrainStateBuilder::ChatLines(CoaIntent intent, bool named, uint32 locale)
{
    COA_ASSERT_WORLD_THREAD();

    std::vector<std::string> names;
    names.emplace_back(CoaVoiceTemplates::ReplyName(intent, named));
    for (char const* related : RelatedReplies(intent))
    {
        std::string name = std::string(REPLY_PREFIX) + related;
        if (names.size() < MAX_LINE_NAMES && std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(std::move(name));
    }

    std::vector<BrainChatLine> lines;
    lines.reserve(names.size() * ROWS_PER_NAME);
    for (std::string const& name : names)
    {
        std::string const shortName = ShortReplyName(name);
        std::vector<std::string> rows = CoaVoiceTemplates::Candidates(name, locale, ROWS_PER_NAME);
        for (std::size_t i = 0; i < rows.size() && i < ROWS_PER_NAME; ++i)
        {
            BrainChatLine line;
            line.key = shortName + "_" + std::to_string(i + 1);
            line.replyName = name;
            line.row = std::move(rows[i]);
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

bool BrainStateBuilder::BuildChatJob(BrainChatFacts const& facts, std::string_view text, BrainChatPlan const& plan,
                                     bool askWho, BrainJob& job)
{
    COA_ASSERT_WORLD_THREAD();

    std::shared_ptr<BrainQuestionSet const> const questions = QuestionRegistry::Current();
    std::shared_ptr<BrainConfig const> const config = BrainConfig::Current();
    return FillChatJob(facts, text, plan, askWho, *questions, config->stateMaxChars, job);
}

bool BrainStateBuilder::BuildTestJob(Player* bot, Player* speaker, std::string const& message, BrainTestJob& out,
                                     std::string& error)
{
    COA_ASSERT_WORLD_THREAD();

    if (!bot || !bot->IsInWorld() || !bot->GetSession() || !GET_PLAYERBOT_AI(bot))
    {
        error = "not an online bot";
        return false;
    }

    bool const named = NameMatch::Mentions(message, FirstName(bot->GetName()));

    BrainChatFacts facts;
    facts.kind = CoaVoiceScopeKind::Whisper;
    facts.speakerLevel = speaker ? speaker->GetLevel() : 60;
    facts.features = BrainFeatures::Extract(message);
    facts.features.namedBots = named ? 1 : 0;
    facts.bots.push_back(BotFacts(bot, speaker, CoaVoiceScopeKind::Whisper, true, named, false));
    facts.speakerGrouped = facts.bots.front().sameGroup;
    facts.speakerGuilded = facts.bots.front().sameGuild;

    BrainChatPlan plan;
    plan.lines = ChatLines(facts.features.intent, named, PlayerbotTextMgr::instance().GetLocalePriority());

    std::shared_ptr<BrainQuestionSet const> const questions = QuestionRegistry::Current();
    if (!FillChatJob(facts, message, plan, false, *questions, BrainConfig::Current()->stateMaxChars, out.job))
    {
        error = "nothing to ask";
        return false;
    }

    out.job.use = BrainUse::Test;
    out.job.lane = BrainLane::Player;
    out.job.situation = "test";
    out.job.cacheKey.clear();
    out.job.botGuid = bot->GetGUID().GetRawValue();
    out.botName = bot->GetName();
    out.scope = CoaVoiceScopeName(CoaVoiceScopeKind::Whisper);
    out.threshold = CoaVoiceConfig::Current()->ThresholdFor(CoaVoiceScopeKind::Whisper);

    CoaVoiceTemplates::Placeholders const placeholders = TestPlaceholders(bot, speaker);
    out.lines.clear();
    for (BrainChatLine const& line : plan.lines)
        out.lines.emplace_back(line.key, placeholders.Fill(line.row));
    return true;
}

std::vector<BrainProbeJob> BrainStateBuilder::BuildProbeJobs(BrainQuestionSet const& questions, uint32 locale)
{
    COA_ASSERT_WORLD_THREAD();

    uint32 const maxChars = BrainConfig::Current()->stateMaxChars;
    std::vector<BrainProbeJob> jobs;
    jobs.reserve(questions.probes.size());
    for (BrainProbeCase const& probe : questions.probes)
    {
        BrainChatFacts facts;
        facts.kind = KindFromName(probe.scope);
        facts.speakerLevel = 30;
        facts.features = BrainFeatures::Extract(probe.message);

        BrainBotFacts bot;
        bot.level = 30;
        bot.className = "Warrior";
        bot.receiver = facts.kind == CoaVoiceScopeKind::Whisper;
        bot.sameGroup = facts.kind == CoaVoiceScopeKind::Party || facts.kind == CoaVoiceScopeKind::Raid;
        bot.sameGuild = facts.kind == CoaVoiceScopeKind::Guild;
        bot.distance = IsLocal(facts.kind) ? "near" : "n/a";
        facts.bots.push_back(bot);
        facts.speakerGrouped = bot.sameGroup;
        facts.speakerGuilded = bot.sameGuild;

        BrainChatPlan plan;
        plan.lines = ChatLines(facts.features.intent, false, locale);

        BrainProbeJob job;
        if (!FillChatJob(facts, probe.message, plan, false, questions, maxChars, job.job))
            continue;

        job.job.use = BrainUse::Probe;
        job.job.lane = BrainLane::Organizer;
        job.job.situation = "probe";
        job.job.cacheKey.clear();
        job.caseId = probe.id;
        job.locale = probe.locale;
        job.expectReply = probe.expectReply;
        job.expectGroup = probe.expectGroup;
        for (BrainChatLine const& line : plan.lines)
            job.lineGroups.emplace_back(line.key, line.replyName);
        jobs.push_back(std::move(job));
    }
    return jobs;
}

uint32 BrainStateBuilder::EmoteFor(std::string_view key)
{
    static constexpr std::pair<std::string_view, uint32> EMOTES[] = {
        {"wave", TEXT_EMOTE_WAVE},   {"bow", TEXT_EMOTE_BOW},     {"nod", TEXT_EMOTE_NOD},
        {"smile", TEXT_EMOTE_SMILE}, {"laugh", TEXT_EMOTE_LAUGH}, {"cheer", TEXT_EMOTE_CHEER},
        {"thank", TEXT_EMOTE_THANK}, {"shrug", TEXT_EMOTE_SHRUG}, {"flex", TEXT_EMOTE_FLEX}};

    for (auto const& [name, emote] : EMOTES)
        if (name == key)
            return emote;
    return 0;
}

char const* BrainStateBuilder::ToneName(int32 level)
{
    switch (level)
    {
        case 0:
            return "hostile";
        case 1:
            return "cold";
        case 2:
            return "neutral";
        case 3:
            return "friendly";
        case 4:
            return "warm";
        default:
            return "unknown";
    }
}

bool BrainStateBuilder::ReadRating(BrainResult const& result, MemorySentiment& sentiment)
{
    if (result.error != BrainError::None)
        return false;

    auto const valid = [&result](std::string_view name, BrainQuestionType type) -> BrainAnswer const*
    {
        BrainAnswer const* const answer = result.Find(name);
        return answer && answer->valid && answer->type == type ? answer : nullptr;
    };
    BrainAnswer const* friendliness = valid("friendliness", BrainQuestionType::Score);
    if (friendliness && (friendliness->level < 0 || friendliness->level > 4))
        friendliness = nullptr;
    BrainAnswer const* const insult = valid("insult", BrainQuestionType::YesNo);
    BrainAnswer const* const thanks = valid("thanks", BrainQuestionType::YesNo);
    if (!friendliness && !insult && !thanks)
        return false;

    if (friendliness)
    {
        sentiment.s = float(friendliness->level - 2) / 2.0f;
        sentiment.c = friendliness->calibrated ? std::clamp(friendliness->p, 0.0f, 1.0f) : 0.3f;
    }
    if (insult)
        sentiment.insult = insult->p >= 0.7f;
    if (thanks)
        sentiment.thanks = thanks->p >= 0.7f;
    sentiment.source = MemorySentimentSource::Brain;
    return true;
}
