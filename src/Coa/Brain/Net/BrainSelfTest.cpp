/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainSelfTest.h"
#include "BrainJson.h"
#include "BrainTypes.h"
#include "IDecisionEngine.h"
#include "SystemOneDialects.h"
#include "SystemOneWire.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t MaxFixtureBytes = 256 * 1024;
constexpr std::size_t MaxFixtures = 256;  // bounds the command's time on the world thread
constexpr double Tolerance = 1e-4;
constexpr float UncalibratedConfidence = 0.3f;

// Copies of data/coa/brain/fixtures/<file>, byte for byte, so that the self-test also runs where the source tree
// is not (CoaBrain.FixtureDir empty: the fixtures are not installed). A run over the source directory notes a
// copy that drifted from its file.
constexpr char const BuiltInVoiceV1[] = R"fixture({
  "fixture": "openrouter-voice-v1",
  "source": "documentation",
  "note": "Built by hand from OpenRouter's documented Decisions API (2026-09-30), not recorded from a live call. Replace it with a recorded fixture (see ../README.md).",
  "dialect": "openrouter",
  "url": "https://openrouter.ai/api/alpha/decisions",
  "status": 200,
  "request": {
    "model": "~typesafe/jev-latest",
    "state": "[realm]\nWorld of Warcraft 3.3.5 private server \"Conquest of Azeroth\" (custom classes). The bots are computer-controlled adventurers of level 1 to 60 who live in the world like players. Players chat mostly in French. A real player's message is described below by its features; the words themselves are shared only when a [player text] section is present. Answer only with the options given.\n[situation]\nplace: a whisper to one bot (b0)\nspeaker: a real player, level 20-29\n[message]\nlanguage: french\nintent: greeting\nalso: question\nlength: short (3 words)\nquestion: yes\nexclamation: no\nshouting: no\nlaughter: no\nnumber: no\nnames a bot: no\n[bots]\nb0: level 23 Barbarian (Berserker), whisper receiver, named: no, same group: no, same guild: no, distance: n/a, fighting: no, answered in the last minute: no, chattiness: high",
    "questions": {
      "reply": {
        "type": "noul",
        "instructions": "Should one of the bots listed in [bots] answer this message in chat? Say yes when a player standing there would answer: the message greets, thanks, asks, compliments, teases or insults these bots, names one of them, or invites an answer. Say no when it is aimed at someone else, is a trade or group post, spam, or chatter that needs no answer.",
        "criteria": {"true": "one bot should answer in chat", "false": "the bots should stay silent"}
      },
      "line": {
        "type": "choice",
        "instructions": "Which line should the answering bot say? Each option starts with its kind in brackets. {name} is the player's name and {me}, {class}, {spec}, {level}, {zone} are the bot's own details; they are filled in afterwards. The lines are in the players' language, mostly French. Pick silent when no line fits the message.",
        "criteria": {
          "greet_1": "[GREETING] Salut {name} !",
          "greet_2": "[GREETING] Yo {name}, bien ou quoi ?",
          "question_1": "[VAGUE ANSWER] Aucune idee, desole.",
          "question_2": "[VAGUE ANSWER] Bonne question, je sais pas trop.",
          "joke_1": "[LAUGHS ALONG] Haha, pas mal.",
          "generic_1": "[NEUTRAL REPLY] Ah d'accord.",
          "silent": "[SILENT] say nothing"
        }
      },
      "tone": {
        "type": "score",
        "instructions": "How warm should the bot's answer be to this message?",
        "criteria": ["hostile or mocking", "cold and curt", "neutral", "friendly", "warm and enthusiastic"]
      },
      "emote": {
        "type": "choice",
        "instructions": "Which emote, if any, should the answering bot play with its line?",
        "criteria": {
          "none": "no emote",
          "wave": "wave",
          "bow": "bow politely",
          "nod": "nod in agreement",
          "smile": "smile",
          "laugh": "laugh",
          "cheer": "cheer",
          "thank": "thank the player",
          "shrug": "shrug",
          "flex": "flex proudly"
        }
      }
    }
  },
  "response": {
    "id": "dec-01JXXXXXXXXXXXXXXX",
    "model": "typesafe/jev-1.13",
    "provider": "TypeSafe",
    "answers": {
      "reply": {"type": "noul", "noul": 0.87},
      "line": {
        "type": "choice",
        "choice": "greet_1",
        "confidence": 0.64,
        "probabilities": {
          "greet_1": 0.64,
          "greet_2": 0.21,
          "question_1": 0.05,
          "question_2": 0.03,
          "joke_1": 0.02,
          "generic_1": 0.03,
          "silent": 0.02
        }
      },
      "tone": {
        "type": "score",
        "score": 3.1,
        "confidence": 0.52,
        "probabilities": {"0": 0.01, "1": 0.03, "2": 0.14, "3": 0.52, "4": 0.30},
        "legend": {
          "0": "hostile or mocking",
          "1": "cold and curt",
          "2": "neutral",
          "3": "friendly",
          "4": "warm and enthusiastic"
        }
      },
      "emote": {
        "type": "choice",
        "choice": "wave",
        "confidence": 0.48,
        "probabilities": {
          "none": 0.30,
          "wave": 0.48,
          "bow": 0.05,
          "nod": 0.04,
          "smile": 0.08,
          "laugh": 0.01,
          "cheer": 0.01,
          "thank": 0.01,
          "shrug": 0.01,
          "flex": 0.01
        }
      }
    },
    "usage": {"input_tokens": 842, "output_tokens": 0, "cost": 0.0000354}
  },
  "expect": {
    "error": "none",
    "model": "typesafe/jev-1.13",
    "answers": {
      "reply": {"valid": true, "calibrated": true, "p": 0.87},
      "line": {
        "valid": true,
        "calibrated": true,
        "choice": "greet_1",
        "p": 0.64,
        "order": ["greet_1", "greet_2", "question_1"]
      },
      "tone": {"valid": true, "level": 3, "p": 0.52, "score": 3.1},
      "emote": {"valid": true, "choice": "wave", "p": 0.48}
    },
    "usage": {"input_tokens": 842, "output_tokens": 0, "usd_micros": 36}
  }
}
)fixture";

constexpr char const BuiltInError402[] = R"fixture({
  "fixture": "openrouter-error-402",
  "source": "documentation",
  "note": "Built by hand from OpenRouter's documented Decisions API (2026-09-30), not recorded from a live call. Replace it with a recorded fixture (see ../README.md).",
  "dialect": "openrouter",
  "url": "https://openrouter.ai/api/alpha/decisions",
  "status": 402,
  "request": {
    "model": "~typesafe/jev-latest",
    "state": "[realm]\nWorld of Warcraft 3.3.5 private server \"Conquest of Azeroth\" (custom classes). The bots are computer-controlled adventurers of level 1 to 60 who live in the world like players. Players chat mostly in French. A real player's message is described below by its features; the words themselves are shared only when a [player text] section is present. Answer only with the options given.\n[situation]\nplace: a whisper to one bot (b0)\nspeaker: a real player, level 20-29\n[message]\nlanguage: french\nintent: greeting\nalso: question\nlength: short (3 words)\nquestion: yes\nexclamation: no\nshouting: no\nlaughter: no\nnumber: no\nnames a bot: no\n[bots]\nb0: level 23 Barbarian (Berserker), whisper receiver, named: no, same group: no, same guild: no, distance: n/a, fighting: no, answered in the last minute: no, chattiness: high",
    "questions": {
      "reply": {
        "type": "noul",
        "instructions": "Should one of the bots listed in [bots] answer this message in chat? Say yes when a player standing there would answer: the message greets, thanks, asks, compliments, teases or insults these bots, names one of them, or invites an answer. Say no when it is aimed at someone else, is a trade or group post, spam, or chatter that needs no answer.",
        "criteria": {"true": "one bot should answer in chat", "false": "the bots should stay silent"}
      }
    }
  },
  "response": {"error": {"code": 402, "message": "Insufficient credits. Add more credits to continue."}},
  "expect": {"error": "exhausted", "detail": "Insufficient credits"}
}
)fixture";

constexpr char const BuiltInInvalidAnswers[] = R"fixture({
  "fixture": "invalid-answers",
  "source": "documentation",
  "note": "Built by hand to test the parser's rejection rules on the documented shape (2026-09-30); no provider sent it. line's choice is not an option, tone's score is 7 on a 5-level scale, reply's noul is 1.5 and emote is missing: no answer is valid, so the whole response is bad.",
  "dialect": "openrouter",
  "url": "https://openrouter.ai/api/alpha/decisions",
  "status": 200,
  "request": {
    "model": "~typesafe/jev-latest",
    "state": "[realm]\nWorld of Warcraft 3.3.5 private server \"Conquest of Azeroth\" (custom classes). The bots are computer-controlled adventurers of level 1 to 60 who live in the world like players. Players chat mostly in French. A real player's message is described below by its features; the words themselves are shared only when a [player text] section is present. Answer only with the options given.\n[situation]\nplace: a whisper to one bot (b0)\nspeaker: a real player, level 20-29\n[message]\nlanguage: french\nintent: greeting\nalso: question\nlength: short (3 words)\nquestion: yes\nexclamation: no\nshouting: no\nlaughter: no\nnumber: no\nnames a bot: no\n[bots]\nb0: level 23 Barbarian (Berserker), whisper receiver, named: no, same group: no, same guild: no, distance: n/a, fighting: no, answered in the last minute: no, chattiness: high",
    "questions": {
      "reply": {
        "type": "noul",
        "instructions": "Should one of the bots listed in [bots] answer this message in chat? Say yes when a player standing there would answer: the message greets, thanks, asks, compliments, teases or insults these bots, names one of them, or invites an answer. Say no when it is aimed at someone else, is a trade or group post, spam, or chatter that needs no answer.",
        "criteria": {"true": "one bot should answer in chat", "false": "the bots should stay silent"}
      },
      "line": {
        "type": "choice",
        "instructions": "Which line should the answering bot say? Each option starts with its kind in brackets. {name} is the player's name and {me}, {class}, {spec}, {level}, {zone} are the bot's own details; they are filled in afterwards. The lines are in the players' language, mostly French. Pick silent when no line fits the message.",
        "criteria": {
          "greet_1": "[GREETING] Salut {name} !",
          "greet_2": "[GREETING] Yo {name}, bien ou quoi ?",
          "question_1": "[VAGUE ANSWER] Aucune idee, desole.",
          "question_2": "[VAGUE ANSWER] Bonne question, je sais pas trop.",
          "joke_1": "[LAUGHS ALONG] Haha, pas mal.",
          "generic_1": "[NEUTRAL REPLY] Ah d'accord.",
          "silent": "[SILENT] say nothing"
        }
      },
      "tone": {
        "type": "score",
        "instructions": "How warm should the bot's answer be to this message?",
        "criteria": ["hostile or mocking", "cold and curt", "neutral", "friendly", "warm and enthusiastic"]
      },
      "emote": {
        "type": "choice",
        "instructions": "Which emote, if any, should the answering bot play with its line?",
        "criteria": {
          "none": "no emote",
          "wave": "wave",
          "bow": "bow politely",
          "nod": "nod in agreement",
          "smile": "smile",
          "laugh": "laugh",
          "cheer": "cheer",
          "thank": "thank the player",
          "shrug": "shrug",
          "flex": "flex proudly"
        }
      }
    }
  },
  "response": {
    "id": "dec-01JINVALIDXXXXXXXXXX",
    "model": "typesafe/jev-1.13",
    "provider": "TypeSafe",
    "answers": {
      "reply": {"type": "noul", "noul": 1.5},
      "line": {"type": "choice", "choice": "greet_7", "confidence": 0.5},
      "tone": {"type": "score", "score": 7}
    },
    "usage": {"input_tokens": 842, "output_tokens": 0, "cost": 0.0000354}
  },
  "expect": {
    "error": "bad-response",
    "answers": {"reply": {"valid": false}, "line": {"valid": false}, "tone": {"valid": false}, "emote": {"valid": false}}
  }
}
)fixture";

struct BuiltInFixture
{
    char const* file;
    std::string_view text;
};

BuiltInFixture const BuiltInFixtures[] = {
    {"openrouter-voice-v1.json", {BuiltInVoiceV1, sizeof(BuiltInVoiceV1) - 1}},
    {"openrouter-error-402.json", {BuiltInError402, sizeof(BuiltInError402) - 1}},
    {"invalid-answers.json", {BuiltInInvalidAnswers, sizeof(BuiltInInvalidAnswers) - 1}},
};

// Report lines go to a GM's chat: printable ASCII only, bounded.
std::string Printable(std::string_view text, std::size_t maxBytes = 120)
{
    std::string out;
    for (char c : text)
    {
        if (out.size() >= maxBytes)
        {
            out += "...";
            break;
        }
        unsigned char const u = static_cast<unsigned char>(c);
        if (c == '\n')
            out += "\\n";
        else
            out += (u < 0x20 || u >= 0x7F) ? '?' : c;
    }
    return out;
}

std::string Num(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

bool Near(double a, double b) { return std::fabs(a - b) <= Tolerance; }

bool QuestionFromWire(SystemOneDialect const& dialect, std::string const& name, BrainJsonValue const& wire,
                      BrainQuestion& question, std::string& why)
{
    question.name = name;
    if (!wire.IsObject())
    {
        why = "question " + name + " is not an object";
        return false;
    }
    std::string const type = wire.GetString(dialect.fType);
    if (type == dialect.typeChoice)
        question.type = BrainQuestionType::Choice;
    else if (type == dialect.typeYesNo)
        question.type = BrainQuestionType::YesNo;
    else if (type == dialect.typeScore)
        question.type = BrainQuestionType::Score;
    else
    {
        why = "question " + name + ": unknown type '" + type + "'";
        return false;
    }
    question.instructions = wire.GetString(dialect.fInstructions);

    BrainJsonValue const* criteria = wire.Find(dialect.fCriteria);
    if (!criteria)
        return true;  // BuildRequest omits the criteria of a YesNo without options; the round trip checks the rest

    switch (question.type)
    {
        case BrainQuestionType::Choice:
            if (!criteria->IsObject())
            {
                why = "question " + name + ": choice criteria must be an object";
                return false;
            }
            for (BrainJsonMember const& option : criteria->members)
            {
                if (!option.value.IsString())
                {
                    why = "question " + name + ": option " + option.key + " has no text";
                    return false;
                }
                question.options.push_back({option.key, option.value.string, std::string()});
            }
            return true;
        case BrainQuestionType::Score:
            if (!criteria->IsArray())
            {
                why = "question " + name + ": score criteria must be an array";
                return false;
            }
            for (std::size_t i = 0; i < criteria->items.size(); ++i)
            {
                if (!criteria->items[i].IsString())
                {
                    why = "question " + name + ": level " + std::to_string(i) + " has no text";
                    return false;
                }
                question.options.push_back({std::to_string(i), criteria->items[i].string, std::string()});
            }
            return true;
        case BrainQuestionType::YesNo:
        {
            BrainJsonValue const* yes = criteria->Find(dialect.keyTrue);
            BrainJsonValue const* no = criteria->Find(dialect.keyFalse);
            if (!criteria->IsObject() || !yes || !no || !yes->IsString() || !no->IsString())
            {
                why = "question " + name + ": yes/no criteria need '" + dialect.keyTrue + "' and '" +
                      dialect.keyFalse + "' texts";
                return false;
            }
            question.options.push_back({"true", yes->string, std::string()});
            question.options.push_back({"false", no->string, std::string()});
            return true;
        }
    }
    return true;
}

bool JobFromRequest(SystemOneDialect const& dialect, BrainJsonValue const& request, BrainJob& job, std::string& why)
{
    BrainJsonValue const* state = request.Find(dialect.fState);
    if (!state || !state->IsString())
    {
        why = "request has no " + dialect.fState + " string";
        return false;
    }
    BrainJsonValue const* questions = request.Find(dialect.fQuestions);
    if (!questions || !questions->IsObject() || questions->members.empty())
    {
        why = "request has no " + dialect.fQuestions + " object";
        return false;
    }
    job.use = BrainUse::Test;
    job.situation = "selftest";
    job.state = state->string;
    for (BrainJsonMember const& member : questions->members)
    {
        BrainQuestion question;
        if (!QuestionFromWire(dialect, member.key, member.value, question, why))
            return false;
        job.questions.push_back(std::move(question));
    }
    return true;
}

// The body as the provider sent it. A recorded fixture keeps a body that was not JSON as {"raw": "<text>"}.
std::string BodyOf(BrainJsonValue const& response)
{
    if (response.IsString())
        return response.string;
    if (response.IsObject() && response.members.size() == 1 && response.members[0].key == "raw" &&
        response.members[0].value.IsString())
        return response.members[0].value.string;
    return BrainJson::Serialize(response);
}

// Ordered deep equality ("" when equal): criteria keep their option order, so order matters.
std::string DiffValue(BrainJsonValue const& want, BrainJsonValue const& got, std::string const& path)
{
    if (want.kind != got.kind)
        return path + ": the fixture and the built request hold different JSON types";
    switch (want.kind)
    {
        case BrainJsonKind::Null:
            return std::string();
        case BrainJsonKind::Bool:
            return want.boolean == got.boolean ? std::string() : path + ": booleans differ";
        case BrainJsonKind::Number:
            return want.number == got.number ? std::string()
                                             : path + ": " + Num(want.number) + " in the fixture, " + Num(got.number) +
                                                   " built";
        case BrainJsonKind::String:
        {
            if (want.string == got.string)
                return std::string();
            std::size_t at = 0;
            while (at < want.string.size() && at < got.string.size() && want.string[at] == got.string[at])
                ++at;
            std::size_t const from = at > 16 ? at - 16 : 0;
            return path + ": differs at byte " + std::to_string(at) + ": fixture \"" +
                   Printable(std::string_view(want.string).substr(from, 48)) + "\", built \"" +
                   Printable(std::string_view(got.string).substr(from, 48)) + "\"";
        }
        case BrainJsonKind::Array:
            if (want.items.size() != got.items.size())
                return path + ": " + std::to_string(want.items.size()) + " items in the fixture, " +
                       std::to_string(got.items.size()) + " built";
            for (std::size_t i = 0; i < want.items.size(); ++i)
            {
                std::string why = DiffValue(want.items[i], got.items[i], path + "[" + std::to_string(i) + "]");
                if (!why.empty())
                    return why;
            }
            return std::string();
        case BrainJsonKind::Object:
            for (std::size_t i = 0; i < want.members.size() || i < got.members.size(); ++i)
            {
                if (i >= want.members.size())
                    return path + ": the built request adds '" + Printable(got.members[i].key, 40) + "'";
                if (i >= got.members.size())
                    return path + ": the built request lacks '" + Printable(want.members[i].key, 40) + "'";
                if (want.members[i].key != got.members[i].key)
                    return path + ": member " + std::to_string(i) + " is '" + Printable(want.members[i].key, 40) +
                           "' in the fixture, '" + Printable(got.members[i].key, 40) + "' built";
                std::string why = DiffValue(want.members[i].value, got.members[i].value,
                                            path + "." + Printable(want.members[i].key, 40));
                if (!why.empty())
                    return why;
            }
            return std::string();
    }
    return std::string();
}

// The same member names, in any order (the provider does not care about the order of these members).
std::string DiffMembers(BrainJsonValue const& want, BrainJsonValue const& got, std::string const& path)
{
    if (!want.IsObject() || !got.IsObject())
        return path + ": not an object";
    for (BrainJsonMember const& member : want.members)
        if (!got.Find(member.key))
            return path + ": the built request lacks '" + Printable(member.key, 40) + "'";
    for (BrainJsonMember const& member : got.members)
        if (!want.Find(member.key))
            return path + ": the built request adds '" + Printable(member.key, 40) + "'";
    for (BrainJsonMember const& member : want.members)
    {
        std::string why = DiffValue(member.value, *got.Find(member.key), path + "." + Printable(member.key, 40));
        if (!why.empty())
            return why;
    }
    return std::string();
}

// The request BuildRequest makes from the rebuilt job against the fixture's: model, state, the questions in order,
// and for each its type, instructions and criteria (keys, order and texts).
std::string DiffRequest(SystemOneDialect const& dialect, BrainJsonValue const& want, BrainJsonValue const& got)
{
    if (!got.IsObject())
        return "the built request is not an object";
    for (BrainJsonMember const& member : want.members)
        if (!got.Find(member.key))
            return "the built request lacks '" + Printable(member.key, 40) + "'";
    for (BrainJsonMember const& member : got.members)
        if (!want.Find(member.key))
            return "the built request adds '" + Printable(member.key, 40) + "'";

    for (BrainJsonMember const& member : want.members)
    {
        BrainJsonValue const& built = *got.Find(member.key);
        if (member.key != dialect.fQuestions)
        {
            std::string why = DiffValue(member.value, built, Printable(member.key, 40));
            if (!why.empty())
                return why;
            continue;
        }
        if (!member.value.IsObject() || !built.IsObject())
            return dialect.fQuestions + ": not an object";
        if (member.value.members.size() != built.members.size())
            return dialect.fQuestions + ": " + std::to_string(member.value.members.size()) + " in the fixture, " +
                   std::to_string(built.members.size()) + " built";
        for (std::size_t i = 0; i < built.members.size(); ++i)
        {
            BrainJsonMember const& fixtureQuestion = member.value.members[i];
            BrainJsonMember const& builtQuestion = built.members[i];
            if (fixtureQuestion.key != builtQuestion.key)
                return dialect.fQuestions + ": question " + std::to_string(i) + " is '" +
                       Printable(fixtureQuestion.key, 40) + "' in the fixture, '" + Printable(builtQuestion.key, 40) +
                       "' built";
            std::string why = DiffMembers(fixtureQuestion.value, builtQuestion.value,
                                          dialect.fQuestions + "." + Printable(fixtureQuestion.key, 40));
            if (!why.empty())
                return why;
        }
    }
    return std::string();
}

BrainAnswer const* FindAnswer(std::vector<BrainAnswer> const& answers, std::string_view name)
{
    for (BrainAnswer const& answer : answers)
        if (answer.name == name)
            return &answer;
    return nullptr;
}

std::string CheckAnswer(std::string const& name, BrainJsonValue const& want, BrainAnswer const* answer)
{
    if (!want.IsObject())
        return "expect.answers." + name + " is not an object";
    bool const valid = answer && answer->valid;
    bool const wantValid = want.GetBool("valid", true);
    if (valid != wantValid)
        return name + ": " + (valid ? "valid" : (answer ? "invalid" : "missing")) + ", expected " +
               (wantValid ? "valid" : "invalid");
    if (!valid)
        return std::string();

    if (BrainJsonValue const* choice = want.Find("choice"); choice && choice->IsString() &&
                                                            choice->string != answer->choice)
        return name + ": choice '" + Printable(answer->choice, 40) + "', expected '" + Printable(choice->string, 40) +
               "'";
    if (BrainJsonValue const* p = want.Find("p"); p && p->IsNumber() && !Near(answer->p, p->number))
        return name + ": p " + Num(answer->p) + ", expected " + Num(p->number);
    if (BrainJsonValue const* level = want.Find("level");
        level && level->IsNumber() && answer->level != int32(std::llround(level->number)))
        return name + ": level " + std::to_string(answer->level) + ", expected " + Num(level->number);
    if (BrainJsonValue const* score = want.Find("score"); score && score->IsNumber() &&
                                                          !Near(answer->score, score->number))
        return name + ": score " + Num(answer->score) + ", expected " + Num(score->number);
    if (BrainJsonValue const* calibrated = want.Find("calibrated");
        calibrated && calibrated->IsBool() && calibrated->boolean != answer->calibrated)
        return name + ": " + (answer->calibrated ? "calibrated" : "uncalibrated") + ", expected " +
               (calibrated->boolean ? "calibrated" : "uncalibrated");
    if (BrainJsonValue const* order = want.Find("order"); order && order->IsArray())
    {
        for (std::size_t i = 0; i < order->items.size(); ++i)
        {
            std::string const expected = order->items[i].IsString() ? order->items[i].string : std::string();
            if (i >= answer->dist.size())
                return name + ": only " + std::to_string(answer->dist.size()) + " probabilities, expected '" +
                       Printable(expected, 40) + "' at rank " + std::to_string(i + 1);
            if (answer->dist[i].first != expected)
                return name + ": rank " + std::to_string(i + 1) + " is '" + Printable(answer->dist[i].first, 40) +
                       "', expected '" + Printable(expected, 40) + "'";
        }
    }
    return std::string();
}

std::string CheckUsage(BrainJsonValue const& want, BrainUsage const& usage)
{
    struct Field
    {
        char const* key;
        uint64 value;
    };
    Field const fields[] = {{"input_tokens", usage.inputTokens},
                            {"output_tokens", usage.outputTokens},
                            {"usd_micros", usage.usdMicros}};
    for (Field const& field : fields)
    {
        BrainJsonValue const* expected = want.Find(field.key);
        if (expected && expected->IsNumber() && double(field.value) != std::floor(expected->number + 0.5))
            return std::string("usage: ") + field.key + " " + std::to_string(field.value) + ", expected " +
                   Num(expected->number);
    }
    if (BrainJsonValue const* reported = want.Find("cost_reported");
        reported && reported->IsBool() && reported->boolean != usage.costReported)
        return std::string("usage: cost ") + (usage.costReported ? "reported" : "not reported") + ", expected " +
               (reported->boolean ? "reported" : "not reported");
    return std::string();
}

std::string CheckError(BrainJsonValue const& expect, BrainError got, std::string const& detail, bool defaultNone)
{
    std::string const wanted = expect.GetString("error", defaultNone ? "none" : "");
    if (!wanted.empty() && wanted != BrainErrorName(got))
    {
        std::string const excerpt = detail.empty() ? std::string() : " (" + Printable(detail, 80) + ")";
        return std::string("error ") + BrainErrorName(got) + excerpt + ", expected " + Printable(wanted, 40);
    }
    if (BrainJsonValue const* part = expect.Find("detail");
        part && part->IsString() && detail.find(part->string) == std::string::npos)
        return "detail \"" + Printable(detail, 80) + "\" lacks \"" + Printable(part->string, 60) + "\"";
    return std::string();
}

// A 200: answers, usage and model through ParseResponse, as an engine reads them.
std::string CheckResponse(SystemOneDialect const& dialect, BrainJob const& job, std::string const& body,
                          BrainJsonValue const& expect)
{
    DecisionReply reply;
    bool const parsed = SystemOneWire::ParseResponse(dialect, job, body, true, UncalibratedConfidence, reply);
    if (parsed != (reply.error == BrainError::None))
        return std::string("ParseResponse returned ") + (parsed ? "true" : "false") + " with error " +
               BrainErrorName(reply.error);

    std::string why = CheckError(expect, reply.error, reply.detail, true);
    if (!why.empty())
        return why;

    if (parsed)
    {
        if (reply.answers.size() != job.questions.size())
            return std::to_string(reply.answers.size()) + " answers for " + std::to_string(job.questions.size()) +
                   " questions";
        for (std::size_t i = 0; i < job.questions.size(); ++i)
            if (reply.answers[i].name != job.questions[i].name)
                return "answer " + std::to_string(i) + " is '" + Printable(reply.answers[i].name, 40) +
                       "', expected '" + Printable(job.questions[i].name, 40) + "'";
    }

    if (BrainJsonValue const* answers = expect.Find("answers"); answers && answers->IsObject())
    {
        for (BrainJsonMember const& member : answers->members)
        {
            why = CheckAnswer(member.key, member.value, FindAnswer(reply.answers, member.key));
            if (!why.empty())
                return why;
        }
    }
    if (BrainJsonValue const* usage = expect.Find("usage"); usage && usage->IsObject())
    {
        why = CheckUsage(*usage, reply.usage);
        if (!why.empty())
            return why;
    }
    if (BrainJsonValue const* model = expect.Find("model"); model && model->IsString() &&
                                                            model->string != reply.providerModel)
        return "model '" + Printable(reply.providerModel, 60) + "', expected '" + Printable(model->string, 60) + "'";
    return std::string();
}

// A recorded fixture may lack "dialect": its file name starts with the engine's dialect ("openrouter-...").
std::string DialectFromFileName(std::string const& stem)
{
    std::string best;
    for (std::string const& name : SystemOneDialects::Names())
        if (stem.size() > name.size() && stem.compare(0, name.size(), name) == 0 && stem[name.size()] == '-' &&
            name.size() > best.size())
            best = name;
    return best;
}

// "" when the fixture passes, else its first failure. `name` gets the fixture's own name.
std::string CheckFixture(std::string const& stem, std::string_view text, std::string& name)
{
    name = stem;
    BrainJsonValue root;
    std::string error;
    if (!BrainJson::Parse(text, root, error, MaxFixtureBytes))
        return "not JSON: " + Printable(error, 80);
    if (!root.IsObject())
        return "not a JSON object";
    if (std::string const own = root.GetString("fixture"); !own.empty())
        name = own;

    std::string dialectName = root.GetString("dialect");
    if (dialectName.empty())
        dialectName = DialectFromFileName(stem);
    if (dialectName.empty())
        return "no \"dialect\"";
    std::shared_ptr<SystemOneDialect const> const dialect = SystemOneDialects::BuiltIn(dialectName);
    if (!dialect)
        return "unknown dialect '" + Printable(dialectName, 40) + "'";

    BrainJsonValue const* request = root.Find("request");
    if (!request || !request->IsObject())
        return "no \"request\" object";
    BrainJob job;
    if (!JobFromRequest(*dialect, *request, job, error))
        return "request: " + error;

    BrainJsonValue const* status = root.Find("status");
    if (!status || !status->IsNumber() || status->number < 100 || status->number > 599 ||
        status->number != std::floor(status->number))
        return "no HTTP \"status\"";
    BrainJsonValue const* response = root.Find("response");
    if (!response)
        return "no \"response\"";
    std::string const body = BodyOf(*response);
    BrainJsonValue const* expectValue = root.Find("expect");
    BrainJsonValue const expect = expectValue && expectValue->IsObject() ? *expectValue : BrainJsonValue::MakeObject();

    int32 const code = int32(status->number);
    std::string why;
    if (code == 200)
        why = CheckResponse(*dialect, job, body, expect);
    else
    {
        std::string detail;
        why = CheckError(expect, SystemOneWire::ClassifyStatus(*dialect, code, body, detail), detail, false);
    }
    if (!why.empty())
        return why;

    // Round trip: the job rebuilt from the fixture must give back the fixture's request.
    DecisionCall call;
    call.job = &job;
    call.sendPlayerText = false;
    call.stateMaxChars = 24000;
    std::string const built =
        SystemOneWire::BuildRequest(*dialect, request->GetString(dialect->fModel), call);
    BrainJsonValue rebuilt;
    if (!BrainJson::Parse(built, rebuilt, error, MaxFixtureBytes))
        return "round trip: the built request is not JSON: " + Printable(error, 80);
    why = DiffRequest(*dialect, *request, rebuilt);
    return why.empty() ? why : "round trip: " + why;
}

void Record(BrainSelfTest::Report& report, std::string const& name, std::string const& why)
{
    if (why.empty())
    {
        ++report.passed;
        report.lines.push_back("PASS " + Printable(name, 64));
    }
    else
    {
        ++report.failed;
        report.lines.push_back("FAIL " + Printable(name, 64) + ": " + Printable(why, 300));
    }
}

void RunOne(BrainSelfTest::Report& report, std::string const& stem, std::string_view text)
{
    std::string name = stem;
    std::string why;
    try
    {
        why = CheckFixture(stem, text, name);
    }
    catch (std::exception const& e)
    {
        why = std::string("exception: ") + Printable(e.what(), 80);
    }
    catch (...)
    {
        why = "unknown exception";
    }
    Record(report, name, why);
}

bool ReadFixture(std::filesystem::path const& path, std::string& text, std::string& why)
{
    std::error_code ec;
    std::uintmax_t const size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        why = "cannot read: " + Printable(ec.message(), 80);
        return false;
    }
    if (size > MaxFixtureBytes)
    {
        why = "larger than 256 KiB";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        why = "cannot open";
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad() || text.size() > MaxFixtureBytes)
    {
        why = "cannot read";
        return false;
    }
    return true;
}

// The *.json files of `dir`, sorted by name; empty with `why` when there are none.
std::vector<std::pair<std::string, std::filesystem::path>> ListFixtures(std::string const& dir, std::string& why)
{
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(dir), ec);
    if (ec)
    {
        why = Printable(dir, 80) + ": " + Printable(ec.message(), 80);
        return files;
    }
    std::filesystem::directory_iterator const end{};
    for (; it != end; it.increment(ec))
    {
        if (ec)
            break;
        std::error_code typeError;
        if (!it->is_regular_file(typeError))
            continue;
        std::filesystem::path const& path = it->path();
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
        if (extension == ".json")
            files.emplace_back(path.filename().string(), path);
    }
    std::sort(files.begin(), files.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    if (files.empty())
        why = Printable(dir, 80) + " has no *.json";
    return files;
}

std::string Stem(std::string const& fileName)
{
    std::size_t const dot = fileName.rfind('.');
    return dot == std::string::npos ? fileName : fileName.substr(0, dot);
}

// Drift guard for the copies above: a run over the source directory notes a copy that no longer matches its file.
void NoteDrift(BrainSelfTest::Report& report, std::string const& fileName, std::string_view fileText)
{
    for (BuiltInFixture const& fixture : BuiltInFixtures)
    {
        if (fileName != fixture.file)
            continue;
        BrainJsonValue fromFile;
        BrainJsonValue fromCopy;
        std::string error;
        if (!BrainJson::Parse(fileText, fromFile, error, MaxFixtureBytes) ||
            !BrainJson::Parse(fixture.text, fromCopy, error, MaxFixtureBytes))
            return;  // the file's own FAIL line says why
        std::string const why = DiffValue(fromFile, fromCopy, Stem(fileName));
        if (!why.empty())
            report.lines.push_back("note: the compiled-in copy of " + Printable(fileName, 64) +
                                   " differs from the file at " + why.substr(0, why.find(": ")) +
                                   "; update BrainSelfTest.cpp");
        return;
    }
}
}  // namespace

BrainSelfTest::Report BrainSelfTest::Run(std::string const& dir)
{
    Report report;
    if (!BrainJson::Available())
    {
        ++report.failed;
        report.lines.push_back("FAIL selftest: Boost.JSON is unavailable (the module was built with Boost < 1.75)");
        return report;
    }

    try
    {
        std::string why = "CoaBrain.FixtureDir is empty";
        std::vector<std::pair<std::string, std::filesystem::path>> files;
        if (!dir.empty())
            files = ListFixtures(dir, why);

        if (files.empty())
        {
            report.lines.push_back("fixtures: " + std::to_string(std::size(BuiltInFixtures)) + " compiled-in (" + why +
                                   ")");
            for (BuiltInFixture const& fixture : BuiltInFixtures)
                RunOne(report, Stem(fixture.file), fixture.text);
            return report;
        }

        std::string header = "fixtures: " + std::to_string(files.size()) + " in " + Printable(dir, 80);
        if (files.size() > MaxFixtures)
        {
            header += " (only the first " + std::to_string(MaxFixtures) + " checked)";
            files.resize(MaxFixtures);
        }
        report.lines.push_back(header);
        for (auto const& [fileName, path] : files)
        {
            std::string text;
            std::string error;
            if (!ReadFixture(path, text, error))
            {
                Record(report, Stem(fileName), error);
                continue;
            }
            RunOne(report, Stem(fileName), text);
            NoteDrift(report, fileName, text);
        }
    }
    catch (std::exception const& e)
    {
        ++report.failed;
        report.lines.push_back(std::string("FAIL selftest: exception: ") + Printable(e.what(), 80));
    }
    catch (...)
    {
        ++report.failed;
        report.lines.push_back("FAIL selftest: unknown exception");
    }
    return report;
}
