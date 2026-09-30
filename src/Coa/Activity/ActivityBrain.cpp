/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityBrain.h"
#include <algorithm>
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
        default:
            return "";
    }
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
    BrainJob job;
    job.tag = tag;
    job.use = BrainUse::Organizer;
    job.lane = BrainLane::Organizer;
    job.situation = SITUATION_NEXT;
    job.deadlineMs = deadlineMs;

    std::vector<std::string> lines;
    RealmLines(scene, lines);
    // The running line goes with the realm: what already runs, against each kind's cap.
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
    for (ActivityKindId const kind : eligible)
        next.options.push_back({ActivityKindName(kind), KindOption(kind), ""});
    next.options.push_back({"none", "nothing new for now", ""});
    next.cacheable = false;
    job.questions.push_back(std::move(next));
    return job;
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
}  // namespace ActivityBrain
