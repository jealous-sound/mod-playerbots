/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityBrain.h"
#include "SharedDefines.h"
#include <algorithm>
#include <cctype>
#include <utility>

namespace
{
char const* ClassWord(uint8 classId)
{
    switch (classId)
    {
        case 1:
            return "warrior";
        case 2:
            return "paladin";
        case 3:
            return "hunter";
        case 4:
            return "rogue";
        case 5:
            return "priest";
        case 6:
            return "death knight";
        case 7:
            return "shaman";
        case 8:
            return "mage";
        case 9:
            return "warlock";
        case 11:
            return "druid";
        default:
            return "adventurer";
    }
}

char const* KindWords(ActivityKindId kind)
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return "duel ring";
        case ActivityKindId::Gang:
            return "gang";
        case ActivityKindId::Dungeon:
            return "dungeon run";
        case ActivityKindId::Hunt:
            return "elite hunt";
        case ActivityKindId::Raid:
            return "outpost raid";
        case ActivityKindId::Party:
            return "inn party";
        case ActivityKindId::Loiter:
            return "city crowd";
        case ActivityKindId::Gurubashi:
            return "arena booty run";
        default:
            return "activity";
    }
}

char const* KindOption(ActivityKindId kind)
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return "a duel ring: a few bots gather at a duel spot near a player and duel each other and passers-by";
        case ActivityKindId::Gang:
            return "a gang: a small group of bots roams a few zones of a player's level, hunting together";
        case ActivityKindId::Dungeon:
            return "a dungeon run: bots form a group for a dungeon of a player's level and invite players in chat";
        case ActivityKindId::Hunt:
            return "an elite hunt: bots group up to kill a named elite near a player, open to players";
        case ActivityKindId::Raid:
            return "an outpost raid: bots raid an enemy quest giver in Hillsbrad and recruit players in chat";
        case ActivityKindId::Party:
            return "an inn party: bots gather at an inn near a player to drink, dance and greet";
        case ActivityKindId::Loiter:
            return "a city crowd: bots loiter at a capital's bank and show off their mounts";
        case ActivityKindId::Gurubashi:
            return "the Gurubashi booty run: bots gather in the arena while the chest is up for grabs";
        default:
            return "";
    }
}

// The English word of a closing emote (the "emote" question's option).
std::string EmoteWord(uint32 emote)
{
    switch (emote)
    {
        case TEXT_EMOTE_CHEER:
            return "cheer";
        case TEXT_EMOTE_ROAR:
            return "roar";
        case TEXT_EMOTE_FLEX:
            return "flex";
        case TEXT_EMOTE_DANCE:
            return "dance";
        case TEXT_EMOTE_VICTORY:
            return "victory";
        case TEXT_EMOTE_APPLAUD:
            return "applaud";
        case TEXT_EMOTE_WAVE:
            return "wave";
        case TEXT_EMOTE_BOW:
            return "bow";
        default:
            return "emote" + std::to_string(emote);
    }
}

// The "line" and "emote" option keys: "<index>_<name>", so that the readers, which get only the option count, find
// the index again.
std::string IndexedKey(std::size_t index, std::string const& name) { return std::to_string(index) + "_" + name; }

std::optional<std::size_t> IndexOfKey(std::string const& key, std::size_t count)
{
    std::size_t const cut = key.find('_');
    if (cut == 0 || cut == std::string::npos || cut > 3)
        return std::nullopt;
    std::size_t index = 0;
    for (std::size_t i = 0; i < cut; ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(key[i])))
            return std::nullopt;
        index = index * 10 + std::size_t(key[i] - '0');
    }
    if (index >= count)
        return std::nullopt;
    return index;
}

bool EqualAsciiI(std::string const& a, std::string const& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [](char x, char y)
                                              {
                                                  return std::tolower(static_cast<unsigned char>(x)) ==
                                                         std::tolower(static_cast<unsigned char>(y));
                                              });
}

// A trait 0..99 as "<n>/10".
std::string Tenths(uint32 trait) { return std::to_string(std::min<uint32>(trait, 99) / 10) + "/10"; }

std::string Join(std::vector<std::string> const& lines)
{
    std::string out;
    for (std::string const& line : lines)
    {
        if (!out.empty())
            out += '\n';
        out += line;
    }
    return out;
}

void RealmLines(ActivityBrain::Scene const& scene, std::vector<std::string>& lines)
{
    lines.emplace_back("[realm]");
    std::string players = "real players online: " + std::to_string(scene.realPlayers);
    if (!scene.playerLevels.empty())
    {
        players += " (levels ";
        std::size_t const shown = std::min<std::size_t>(scene.playerLevels.size(), 8);
        for (std::size_t i = 0; i < shown; ++i)
            players += (i ? ", " : "") + std::to_string(uint32(scene.playerLevels[i]));
        players += ")";
    }
    lines.push_back(std::move(players));
    lines.push_back("free bots: " + std::to_string(scene.freeBots));
    lines.push_back("bots in activities: " + std::to_string(scene.participants));
    lines.push_back("hour: " + std::to_string(scene.hourOfDay));
}

BrainQuestion YesNoQuestion(std::string name, std::string instructions, std::string yes, std::string no)
{
    BrainQuestion question;
    question.name = std::move(name);
    question.type = BrainQuestionType::YesNo;
    question.instructions = std::move(instructions);
    question.options = {{"true", std::move(yes), ""}, {"false", std::move(no), ""}};
    question.cacheable = false;
    return question;
}

BrainAnswer const* ValidAnswer(BrainResult const& result, std::string const& name, BrainQuestionType type)
{
    if (result.error != BrainError::None)
        return nullptr;
    BrainAnswer const* const answer = result.Find(name);
    return answer && answer->valid && answer->type == type ? answer : nullptr;
}
}  // namespace

namespace ActivityBrain
{
BrainJob BuildNextKindJob(Scene const& scene, std::vector<ActivityKindId> const& eligible, uint64 tag,
                          uint64 deadlineMs)
{
    std::vector<NextOption> options;
    options.reserve(eligible.size());
    for (ActivityKindId const kind : eligible)
        options.push_back({kind, ""});
    return BuildNextJob(scene, options, tag, deadlineMs);
}

Choice ReadNextKind(BrainResult const& result, std::vector<ActivityKindId> const& eligible)
{
    Choice choice;
    BrainAnswer const* const answer = ValidAnswer(result, "next", BrainQuestionType::Choice);
    if (!answer)
        return choice;
    if (answer->choice == "none")
    {
        choice.valid = true;
        choice.none = true;
        return choice;
    }
    std::optional<ActivityKindId> const kind = ActivityKindFromName(answer->choice);
    if (kind && std::find(eligible.begin(), eligible.end(), *kind) != eligible.end())
    {
        choice.valid = true;
        choice.kind = *kind;
    }
    return choice;
}

BrainJob BuildJoinJob(ActivityKindId kind, Scene const& scene, std::string const& where, uint8 minLevel,
                      uint8 maxLevel, std::vector<Candidate> const& candidates, uint64 tag, uint64 deadlineMs)
{
    BrainJob job;
    job.tag = tag;
    job.use = BrainUse::Organizer;
    job.lane = BrainLane::Organizer;
    job.situation = SITUATION_JOIN;
    job.deadlineMs = deadlineMs;

    std::vector<std::string> lines;
    RealmLines(scene, lines);
    lines.emplace_back("[activity]");
    lines.push_back(std::string("kind: ") + KindWords(kind));
    lines.push_back("where: " + (where.empty() ? std::string("unknown") : where));
    lines.push_back("levels: " + std::to_string(uint32(minLevel)) + "-" + std::to_string(uint32(maxLevel)));
    lines.emplace_back("[candidates]");

    std::size_t const count = std::min<std::size_t>(candidates.size(), ActivityThreshold::MAX_JOIN_QUESTIONS);
    for (std::size_t i = 0; i < count; ++i)
    {
        Candidate const& c = candidates[i];
        std::string const b = "b" + std::to_string(i);
        lines.push_back(b + ": level " + std::to_string(uint32(c.level)) + " " + ClassWord(c.classId) + ", bravery " +
                        Tenths(c.bravery) + ", sociability " + Tenths(c.sociability) + ", pvp " +
                        Tenths(c.pvpAppetite) + ", " +
                        (c.distance < 0 ? std::string("far away") : std::to_string(c.distance) + " yards away"));
        job.questions.push_back(YesNoQuestion("join_" + std::to_string(i),
                                              "Would " + b + " want to join this activity now, given its character?",
                                              b + " joins", b + " keeps doing what it was doing"));
    }
    job.state = Join(lines);
    return job;
}

std::vector<int8> ReadJoin(BrainResult const& result, std::size_t count)
{
    std::vector<int8> decisions(count, -1);
    for (std::size_t i = 0; i < count; ++i)
        if (BrainAnswer const* answer = ValidAnswer(result, "join_" + std::to_string(i), BrainQuestionType::YesNo))
            decisions[i] = int8(answer->p >= 0.5f ? 1 : 0);
    return decisions;
}

BrainJob BuildDuelJob(DuelFeatures const& features, uint64 botGuidRaw, uint64 tag, uint64 deadlineMs)
{
    BrainJob job;
    job.tag = tag;
    job.use = BrainUse::Duel;
    job.lane = BrainLane::Player;
    job.botGuid = botGuidRaw;
    job.situation = SITUATION_DUEL;
    job.deadlineMs = deadlineMs;

    std::vector<std::string> lines;
    lines.emplace_back("[duel]");
    lines.push_back("the bot: level " + std::to_string(uint32(features.botLevel)) + " " +
                    ClassWord(features.botClass) + ", health " + std::to_string(uint32(features.botHealthPct)) +
                    "%, bravery " + Tenths(features.bravery) + ", at a duel ring: " + (features.inRing ? "yes" : "no"));
    lines.push_back("the challenger: a player, level " + std::to_string(uint32(features.challengerLevel)) + " " +
                    ClassWord(features.challengerClass));
    lines.push_back(std::string("history: ") + (features.rival ? "rivals" : features.friendly ? "friends" : "none"));
    lines.push_back("duels this player fought against bots today: " + std::to_string(features.duelsToday));
    job.state = Join(lines);

    job.questions.push_back(YesNoQuestion("duel_accept", "Should the bot accept this duel challenge?",
                                          "the bot accepts the duel", "the bot declines the duel"));
    return job;
}

std::optional<bool> ReadDuel(BrainResult const& result, float threshold)
{
    BrainAnswer const* const answer = ValidAnswer(result, "duel_accept", BrainQuestionType::YesNo);
    if (!answer)
        return std::nullopt;
    return answer->p >= threshold;
}

// --- M5 ---

BrainJob BuildNextJob(Scene const& scene, std::vector<NextOption> const& options, uint64 tag, uint64 deadlineMs)
{
    BrainJob job;
    job.tag = tag;
    job.use = BrainUse::Organizer;
    job.lane = BrainLane::Organizer;
    job.situation = SITUATION_NEXT;
    job.deadlineMs = deadlineMs;

    std::vector<std::string> lines;
    RealmLines(scene, lines);
    // The running line goes with the realm: what already runs, per kind.
    std::string running = "running:";
    for (uint8 kind = uint8(ActivityKindId::None) + 1; kind < uint8(ActivityKindId::Count); ++kind)
        running += std::string(kind > 1 ? ", " : " ") + ActivityKindName(ActivityKindId(kind)) + " " +
                   std::to_string(scene.running[kind]);
    lines.insert(lines.end() - 1, std::move(running));
    job.state = Join(lines);

    BrainQuestion next;
    next.name = "next";
    next.type = BrainQuestionType::Choice;
    next.instructions = "Pick what the bots of this realm should start next, if anything. Prefer something that suits "
                        "the levels of the players online; choose none when the realm already feels busy.";
    for (NextOption const& option : options)
    {
        std::string key = ActivityKindName(option.kind);
        std::string text = KindOption(option.kind);
        if (!option.target.empty())
        {
            key += "_" + option.target;
            text += ", going for " + option.target;
        }
        next.options.push_back({std::move(key), std::move(text), ""});
    }
    next.options.push_back({"none", "nothing new for now", ""});
    next.cacheable = false;
    job.questions.push_back(std::move(next));
    return job;
}

NextPick ReadNext(BrainResult const& result, std::vector<NextOption> const& options)
{
    NextPick pick;
    BrainAnswer const* const answer = ValidAnswer(result, "next", BrainQuestionType::Choice);
    if (!answer)
        return pick;
    if (answer->choice == "none")
    {
        pick.valid = true;
        pick.none = true;
        return pick;
    }
    for (std::size_t i = 0; i < options.size(); ++i)
    {
        std::string key = ActivityKindName(options[i].kind);
        if (!options[i].target.empty())
            key += "_" + options[i].target;
        if (EqualAsciiI(key, answer->choice))
        {
            pick.valid = true;
            pick.index = int32(i);
            return pick;
        }
    }
    return pick;
}

BrainJob BuildLineJob(ActivityKindId kind, std::string const& where, std::vector<std::string> const& keys,
                      std::vector<std::string> const& englishRows, uint64 tag, uint64 deadlineMs)
{
    BrainJob job;
    job.tag = tag;
    job.use = BrainUse::Organizer;
    job.lane = BrainLane::Organizer;
    job.situation = SITUATION_LINE;
    job.deadlineMs = deadlineMs;

    std::vector<std::string> lines;
    lines.emplace_back("[recruitment]");
    lines.push_back(std::string("kind: ") + KindWords(kind));
    lines.push_back("where: " + (where.empty() ? std::string("unknown") : where));
    job.state = Join(lines);

    BrainQuestion line;
    line.name = "line";
    line.type = BrainQuestionType::Choice;
    line.instructions = "Pick the recruitment message a friendly adventurer would post now.";
    for (std::size_t i = 0; i < keys.size(); ++i)
        line.options.push_back({IndexedKey(i, keys[i]), i < englishRows.size() ? englishRows[i] : keys[i], ""});
    line.cacheable = false;
    job.questions.push_back(std::move(line));
    return job;
}

std::optional<std::size_t> ReadLine(BrainResult const& result, std::size_t count)
{
    BrainAnswer const* const answer = ValidAnswer(result, "line", BrainQuestionType::Choice);
    if (!answer)
        return std::nullopt;
    return IndexOfKey(answer->choice, count);
}

BrainJob BuildRetreatJob(ActivityKindId kind, RetreatFeatures const& features, uint64 tag, uint64 deadlineMs)
{
    BrainJob job;
    job.tag = tag;
    job.use = BrainUse::Organizer;
    job.lane = BrainLane::Organizer;
    job.situation = SITUATION_RETREAT;
    job.deadlineMs = deadlineMs;

    std::vector<std::string> lines;
    if (kind == ActivityKindId::Raid)
        lines.emplace_back("[raid]");
    else
    {
        lines.emplace_back("[activity]");
        lines.push_back(std::string("kind: ") + KindWords(kind));
    }
    lines.push_back("bots: " + std::to_string(features.botsIn) + ", alive: " + std::to_string(features.botsAlive) +
                    ", players: " + std::to_string(features.playersIn));
    lines.push_back("target health: " + std::to_string(uint32(features.targetHealthPct)) + "%");
    lines.push_back("engaged for: " + std::to_string(features.engagedSeconds) + " seconds");
    lines.push_back(std::string("enemy player near: ") + (features.defenderNear ? "yes" : "no"));
    job.state = Join(lines);

    job.questions.push_back(YesNoQuestion("retreat", "Should the raid fall back and regroup now?",
                                          "the raid falls back to its rally point", "the raid keeps fighting"));
    return job;
}

std::optional<bool> ReadRetreat(BrainResult const& result)
{
    BrainAnswer const* const answer = ValidAnswer(result, "retreat", BrainQuestionType::YesNo);
    if (!answer)
        return std::nullopt;
    return answer->p >= 0.5f;
}

BrainJob BuildEmoteJob(ActivityKindId kind, bool success, std::vector<uint32> const& emotes, uint64 tag,
                       uint64 deadlineMs)
{
    BrainJob job;
    job.tag = tag;
    job.use = BrainUse::Organizer;
    job.lane = BrainLane::Organizer;
    job.situation = SITUATION_EMOTE;
    job.deadlineMs = deadlineMs;

    std::vector<std::string> lines;
    lines.emplace_back("[activity]");
    lines.push_back(std::string("kind: ") + KindWords(kind));
    lines.push_back(std::string("result: ") + (success ? "success" : "failure"));
    job.state = Join(lines);

    BrainQuestion emote;
    emote.name = "emote";
    emote.type = BrainQuestionType::Choice;
    emote.instructions = "Pick the emote the bots play together to close this activity.";
    for (std::size_t i = 0; i < emotes.size(); ++i)
    {
        std::string const word = EmoteWord(emotes[i]);
        emote.options.push_back({IndexedKey(i, word), word, ""});
    }
    emote.cacheable = false;
    job.questions.push_back(std::move(emote));
    return job;
}

std::optional<std::size_t> ReadEmote(BrainResult const& result, std::size_t count)
{
    BrainAnswer const* const answer = ValidAnswer(result, "emote", BrainQuestionType::Choice);
    if (!answer)
        return std::nullopt;
    return IndexOfKey(answer->choice, count);
}
}  // namespace ActivityBrain
