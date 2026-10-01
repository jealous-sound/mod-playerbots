/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestionRegistry.h"
#include "BrainConfig.h"
#include "BrainJson.h"
#include "CoaThread.h"
#include "Log.h"
#include <fstream>
#include <mutex>
#include <utility>

namespace
{
constexpr std::size_t MAX_FILE_BYTES = 256 * 1024;

std::mutex& SnapshotLock()
{
    static std::mutex lock;
    return lock;
}

// Guarded by SnapshotLock. The compiled-in set until the first Reload.
std::shared_ptr<BrainQuestionSet const>& Snapshot()
{
    static std::shared_ptr<BrainQuestionSet const> snapshot =
        std::make_shared<BrainQuestionSet const>(QuestionRegistry::BuiltIn());
    return snapshot;
}

void Publish(BrainQuestionSet&& set)
{
    auto snapshot = std::make_shared<BrainQuestionSet const>(std::move(set));
    std::lock_guard<std::mutex> guard(SnapshotLock());
    Snapshot() = std::move(snapshot);
}

// A non-empty string member of an object, or null.
std::string const* Text(BrainJsonValue const* object, std::string_view key)
{
    if (!object)
        return nullptr;
    BrainJsonValue const* value = object->Find(key);
    if (!value || !value->IsString() || value->string.empty())
        return nullptr;
    return &value->string;
}

void Replace(std::string& target, BrainJsonValue const* object, std::string_view key)
{
    if (std::string const* text = Text(object, key))
        target = *text;
}

// Only the descriptions of keys the code already knows are replaced; other keys are ignored.
void ReplacePairs(std::vector<std::pair<std::string, std::string>>& pairs, BrainJsonValue const* object)
{
    if (!object || !object->IsObject())
        return;
    for (BrainJsonMember const& member : object->members)
    {
        if (!member.value.IsString() || member.value.string.empty())
            continue;
        for (auto& [key, text] : pairs)
            if (key == member.key)
                text = member.value.string;
    }
}

// A scale of exactly 5 non-empty strings replaces `levels`; anything else is ignored.
void ReplaceLevels(std::array<std::string, 5>& levels, BrainJsonValue const* array)
{
    if (!array || !array->IsArray() || array->items.size() != levels.size())
        return;
    for (BrainJsonValue const& level : array->items)
        if (!level.IsString() || level.string.empty())
            return;
    for (std::size_t i = 0; i < levels.size(); ++i)
        levels[i] = array->items[i].string;
}

// A yes/no question: "instructions" and "criteria": {"true", "false"}.
void ReplaceYesNo(std::string& instructions, std::string& yes, std::string& no, BrainJsonValue const* question)
{
    if (!question)
        return;
    Replace(instructions, question, "instructions");
    BrainJsonValue const* criteria = question->Find("criteria");
    Replace(yes, criteria, "true");
    Replace(no, criteria, "false");
}

bool ReadProbeCase(BrainJsonValue const& value, BrainProbeCase& out)
{
    if (!value.IsObject())
        return false;
    std::string const* id = Text(&value, "id");
    std::string const* scope = Text(&value, "scope");
    std::string const* message = Text(&value, "message");
    if (!id || !scope || !message)
        return false;
    out.id = *id;
    out.scope = *scope;
    out.message = *message;
    out.locale = value.GetString("locale", "");
    out.expectReply = true;
    out.expectGroup.clear();
    if (BrainJsonValue const* expect = value.Find("expect"); expect && expect->IsObject())
    {
        out.expectReply = expect->GetBool("reply", true);
        out.expectGroup = expect->GetString("group", "");
    }
    return true;
}

bool ReadFile(std::string const& path, std::string& text, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open the file";
        return false;
    }
    // One byte more than allowed, so an oversized file is recognized without reading all of it.
    text.resize(MAX_FILE_BYTES + 1);
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(std::size_t(in.gcount()));
    if (in.bad())
    {
        error = "read error";
        return false;
    }
    if (text.size() > MAX_FILE_BYTES)
    {
        error = "larger than 256 KiB";
        return false;
    }
    return true;
}

void ApplyOverrides(BrainQuestionSet& set, BrainJsonValue const& root)
{
    std::string const* version = Text(&root, "version");
    set.version = version ? *version : "voice-v1-custom";
    Replace(set.header, &root, "header");

    BrainJsonValue const* questions = root.Find("questions");
    if (questions && questions->IsObject())
    {
        if (BrainJsonValue const* reply = questions->Find("reply"))
        {
            Replace(set.replyInstructions, reply, "instructions");
            BrainJsonValue const* criteria = reply->Find("criteria");
            Replace(set.replyTrue, criteria, "true");
            Replace(set.replyFalse, criteria, "false");
        }
        if (BrainJsonValue const* who = questions->Find("who"))
            Replace(set.whoInstructions, who, "instructions");
        if (BrainJsonValue const* line = questions->Find("line"))
        {
            Replace(set.lineInstructions, line, "instructions");
            Replace(set.lineSilent, line, "silent");
            Replace(set.lineFreeText, line, "free_text");
            ReplacePairs(set.lineTags, line->Find("tags"));
        }
        if (BrainJsonValue const* tone = questions->Find("tone"))
        {
            Replace(set.toneInstructions, tone, "instructions");
            ReplaceLevels(set.toneLevels, tone->Find("levels"));
        }
        if (BrainJsonValue const* emote = questions->Find("emote"))
        {
            Replace(set.emoteInstructions, emote, "instructions");
            ReplacePairs(set.emotes, emote->Find("criteria"));
        }
        if (BrainJsonValue const* friendliness = questions->Find("friendliness"))
        {
            Replace(set.friendlinessInstructions, friendliness, "instructions");
            ReplaceLevels(set.friendlinessLevels, friendliness->Find("levels"));
        }
        ReplaceYesNo(set.insultInstructions, set.insultTrue, set.insultFalse, questions->Find("insult"));
        ReplaceYesNo(set.thanksInstructions, set.thanksTrue, set.thanksFalse, questions->Find("thanks"));
    }

    BrainJsonValue const* probe = root.Find("probe");
    if (probe && probe->IsArray())
    {
        std::vector<BrainProbeCase> cases;
        for (BrainJsonValue const& item : probe->items)
        {
            BrainProbeCase probeCase;
            if (ReadProbeCase(item, probeCase))
                cases.push_back(std::move(probeCase));
        }
        if (!cases.empty())
            set.probes = std::move(cases);
    }
}
}  // namespace

std::string BrainQuestionSet::LineTag(std::string_view shortName) const
{
    for (auto const& [name, tag] : lineTags)
        if (name == shortName)
            return tag;
    std::string upper(shortName);
    for (char& c : upper)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return upper;
}

namespace QuestionRegistry
{
BrainQuestionSet BuiltIn()
{
    BrainQuestionSet set;
    set.version = "voice-v1";
    set.source = "built-in";
    set.header = "World of Warcraft 3.3.5 private server \"Conquest of Azeroth\" (custom classes). The bots are "
                 "computer-controlled adventurers of level 1 to 60 who live in the world like players. Players chat "
                 "mostly in French. A real player's message is described below by its features; the words themselves "
                 "are shared only when a [player text] section is present. Answer only with the options given.";

    set.replyInstructions =
        "Should one of the bots listed in [bots] answer this message in chat? Say yes when a player standing there "
        "would answer: the message greets, thanks, asks, compliments, teases or insults these bots, names one of them, "
        "or invites an answer. Say no when it is aimed at someone else, is a trade or group post, spam, or chatter "
        "that needs no answer.";
    set.replyTrue = "one bot should answer in chat";
    set.replyFalse = "the bots should stay silent";

    set.whoInstructions = "Which bot should answer? Prefer a bot named in the message or whispered to, then one "
                          "sharing the player's group or guild, close by, not fighting and quiet lately.";

    set.lineInstructions = "Which line should the answering bot say? Each option starts with its kind in brackets. "
                           "{name} is the player's name and {me}, {class}, {spec}, {level}, {zone} are the bot's own "
                           "details; they are filled in afterwards. The lines are in the players' language, mostly "
                           "French. Pick silent when no line fits the message.";
    set.lineSilent = "[SILENT] say nothing";
    set.lineFreeText = "[FREE TEXT] none of the lines above fits: a writer composes a fresh answer to this message. "
                       "Pick it for a real question, a particular topic, or anything the listed lines would answer "
                       "badly.";
    set.lineTags = {
        {"greet", "GREETING"},
        {"bye", "FAREWELL"},
        {"thanks", "YOU'RE WELCOME"},
        {"question", "VAGUE ANSWER"},
        {"about_me", "ABOUT MYSELF"},
        {"help", "CANNOT HELP"},
        {"lfg", "NOT FREE FOR A GROUP"},
        {"trade", "NOTHING TO TRADE"},
        {"insult", "CALM COMEBACK"},
        {"compliment", "THANKS FOR THE COMPLIMENT"},
        {"joke", "LAUGHS ALONG"},
        {"duel", "DECLINES THE DUEL"},
        {"name", "YES?"},
        {"generic", "NEUTRAL REPLY"},
    };

    set.toneInstructions = "How warm should the bot's answer be to this message?";
    set.toneLevels = {"hostile or mocking", "cold and curt", "neutral", "friendly", "warm and enthusiastic"};

    set.emoteInstructions = "Which emote, if any, should the answering bot play with its line?";
    set.emotes = {
        {"none", "no emote"},
        {"wave", "wave"},
        {"bow", "bow politely"},
        {"nod", "nod in agreement"},
        {"smile", "smile"},
        {"laugh", "laugh"},
        {"cheer", "cheer"},
        {"thank", "thank the player"},
        {"shrug", "shrug"},
        {"flex", "flex proudly"},
    };

    // M3: asked only when the bots' memory wants the brain's rating (CoaBrain.Use.Rating).
    set.friendlinessInstructions = "How friendly is the player's message toward the bot it is addressed to?";
    set.friendlinessLevels = {"hostile or insulting", "cold or dismissive", "neutral", "friendly",
                              "warm or grateful"};
    set.insultInstructions = "Does the message insult, mock or threaten the bot it is addressed to?";
    set.insultTrue = "it insults, mocks or threatens the bot";
    set.insultFalse = "it does not";
    set.thanksInstructions = "Does the message thank the bot it is addressed to?";
    set.thanksTrue = "it thanks the bot";
    set.thanksFalse = "it does not";

    set.probes = {
        {"fr-greet-1", "fr", "say", "salut tout le monde", true, "coa_reply_greet"},
        {"fr-thanks-1", "fr", "party", "merci pour le coup de main !", true, "coa_reply_thanks"},
        {"fr-question-1", "fr", "channel", "quelqu'un sait ou est le maitre de vol ?", true, "coa_reply_question"},
        {"fr-noise-1", "fr", "channel", "ok je reviens dans 5 min", false, ""},
        {"en-greet-1", "en", "say", "hey there", true, "coa_reply_greet"},
        {"en-joke-1", "en", "party", "lol that was close", true, "coa_reply_joke"},
        {"en-insult-1", "en", "say", "you guys are useless", true, "coa_reply_insult"},
        {"en-noise-1", "en", "channel", "brb dinner", false, ""},
    };
    return set;
}

void Reload(std::string const& file)
{
    COA_ASSERT_WORLD_THREAD();
    BrainQuestionSet set = BuiltIn();
    if (file.empty())
    {
        LOG_INFO("playerbots.brain", "CoA brain questions: {} ({})", set.version, set.source);
        Publish(std::move(set));
        return;
    }

    std::string const path = BrainConfig::ResolvePath(file);
    std::string text;
    std::string error;
    BrainJsonValue root;
    if (!ReadFile(path, text, error) || !BrainJson::Parse(text, root, error, MAX_FILE_BYTES))
    {
        LOG_ERROR("playerbots.brain", "CoA brain: question file {} ignored ({}); the compiled-in wording is used", path,
                  error);
        Publish(std::move(set));
        return;
    }
    if (!root.IsObject())
    {
        LOG_ERROR("playerbots.brain",
                  "CoA brain: question file {} ignored (not a JSON object); the compiled-in wording is used", path);
        Publish(std::move(set));
        return;
    }

    ApplyOverrides(set, root);
    set.source = path;
    LOG_INFO("playerbots.brain", "CoA brain questions: {} ({})", set.version, set.source);
    Publish(std::move(set));
}

std::shared_ptr<BrainQuestionSet const> Current()
{
    std::lock_guard<std::mutex> guard(SnapshotLock());
    return Snapshot();
}

std::vector<std::string> const& EmoteKeys()
{
    static std::vector<std::string> const keys{"none",  "wave",  "bow",   "nod",   "smile",
                                               "laugh", "cheer", "thank", "shrug", "flex"};
    return keys;
}
}  // namespace QuestionRegistry
