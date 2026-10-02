/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ActivityConfig.h"
#include "Config.h"
#include "Log.h"
#include "Util.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <utility>

namespace
{
std::string const Prefix = "AiPlayerbot.CoaActivity.";

std::mutex& SnapshotLock()
{
    static std::mutex lock;
    return lock;
}

// Guarded by SnapshotLock. Holds a disabled default until the first Publish.
std::shared_ptr<ActivityConfig const>& Snapshot()
{
    static std::shared_ptr<ActivityConfig const> snapshot = std::make_shared<ActivityConfig const>();
    return snapshot;
}

std::atomic<bool> enabledFlag{false};

// Signed reads, so that a negative value in the conf is clamped instead of wrapping around.
uint32 Int(std::string const& key, int32 def, int32 low, int32 high)
{
    return uint32(std::clamp(sConfigMgr->GetOption<int32>(Prefix + key, def), low, high));
}

bool Bool(std::string const& key, bool def) { return sConfigMgr->GetOption<bool>(Prefix + key, def); }

std::string Trimmed(std::string const& text)
{
    std::size_t const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::vector<std::string> Split(std::string const& text, char separator)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true)
    {
        std::size_t const end = text.find(separator, start);
        out.push_back(Trimmed(text.substr(start, end == std::string::npos ? std::string::npos : end - start)));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

bool ParseFloat(std::string const& text, float& out)
{
    if (text.empty())
        return false;
    char* end = nullptr;
    float const value = std::strtof(text.c_str(), &end);
    if (!end || *end || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

bool ParseUInt(std::string const& text, uint32 max, uint32& out)
{
    if (text.empty() || !std::isdigit(static_cast<unsigned char>(text[0])))
        return false;
    char* end = nullptr;
    unsigned long const value = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end || value > max)
        return false;
    out = uint32(value);
    return true;
}

bool ValidSpotName(std::string const& name)
{
    if (name.empty() || name.size() > 24)
        return false;
    for (char const c : name)
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return false;
    return true;
}

// One `name:team:map:x:y:z:minLevel:maxLevel` entry; `why` says what is wrong.
bool ParseSpot(std::string const& entry, ActivityDuelSpot& spot, std::string& why)
{
    std::vector<std::string> const fields = Split(entry, ':');
    if (fields.size() != 8)
    {
        why = "expected name:team:map:x:y:z:minLevel:maxLevel";
        return false;
    }

    spot.name = Lower(fields[0]);
    if (!ValidSpotName(spot.name))
    {
        why = "the name must be 1 to 24 characters among a-z, 0-9 and _";
        return false;
    }

    std::string const team = Lower(fields[1]);
    if (team == "alliance")
        spot.team = 0;
    else if (team == "horde")
        spot.team = 1;
    else
    {
        why = "the team must be alliance or horde";
        return false;
    }

    uint32 minLevel = 0;
    uint32 maxLevel = 0;
    if (!ParseUInt(fields[2], 0xFFFFFFFFu, spot.mapId) || !ParseFloat(fields[3], spot.x) ||
        !ParseFloat(fields[4], spot.y) || !ParseFloat(fields[5], spot.z))
    {
        why = "the map must be a number and x, y, z decimal numbers";
        return false;
    }
    if (!ParseUInt(fields[6], 255, minLevel) || !ParseUInt(fields[7], 255, maxLevel) || !minLevel || !maxLevel ||
        minLevel > maxLevel)
    {
        why = "the levels must be 1 to 255 with minLevel <= maxLevel";
        return false;
    }
    spot.minLevel = uint8(minLevel);
    spot.maxLevel = uint8(maxLevel);
    return true;
}

std::vector<ActivityDuelSpot> ParseSpots(std::string const& text, bool logErrors)
{
    std::vector<ActivityDuelSpot> spots;
    for (std::string const& entry : Split(text, ';'))
    {
        if (entry.empty())
            continue;

        ActivityDuelSpot spot;
        std::string why;
        if (!ParseSpot(entry, spot, why))
        {
            if (logErrors)
                LOG_ERROR("playerbots.activity", "AiPlayerbot.CoaActivity.Duel.Spots: entry '{}' skipped ({})", entry,
                          why);
            continue;
        }
        bool const duplicate = std::any_of(spots.begin(), spots.end(),
                                           [&spot](ActivityDuelSpot const& other) { return other.name == spot.name; });
        if (duplicate)
        {
            if (logErrors)
                LOG_ERROR("playerbots.activity",
                          "AiPlayerbot.CoaActivity.Duel.Spots: entry '{}' skipped (the name {} is used twice)", entry,
                          spot.name);
            continue;
        }
        spots.push_back(std::move(spot));
    }
    return spots;
}

// "alliance", "horde", and with `anyAllowed` "any" (TEAM_NEUTRAL).
bool ParseTeam(std::string const& text, bool anyAllowed, uint8& team)
{
    std::string const word = Lower(text);
    if (word == "alliance")
        team = 0;
    else if (word == "horde")
        team = 1;
    else if (anyAllowed && word == "any")
        team = 2;
    else
        return false;
    return true;
}

// The M5 lists: `name:...` entries separated by ';'. `parse` fills one entry from its fields or says why not; a
// malformed entry or a name used twice is skipped with one LOG_ERROR naming it (when `logErrors`).
template <typename Entry, typename Parse>
std::vector<Entry> ParseEntries(char const* key, std::string const& text, bool logErrors, Parse parse)
{
    std::vector<Entry> entries;
    for (std::string const& raw : Split(text, ';'))
    {
        if (raw.empty())
            continue;

        std::vector<std::string> const fields = Split(raw, ':');
        Entry entry;
        std::string why;
        if (!ValidSpotName(Lower(fields[0])))
            why = "the name must be 1 to 24 characters among a-z, 0-9 and _";
        else
        {
            entry.name = Lower(fields[0]);
            parse(fields, entry, why);
        }
        if (why.empty() && std::any_of(entries.begin(), entries.end(),
                                       [&entry](Entry const& other) { return other.name == entry.name; }))
            why = "the name " + entry.name + " is used twice";
        if (!why.empty())
        {
            if (logErrors)
                LOG_ERROR("playerbots.activity", "AiPlayerbot.CoaActivity.{}: entry '{}' skipped ({})", key, raw, why);
            continue;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

// Hunt.Targets: name:spawn:team.
std::vector<ActivityHuntTarget> ParseHuntTargets(std::string const& text, bool logErrors,
                                               char const* key = "Hunt.Targets")
{
    return ParseEntries<ActivityHuntTarget>(
        key, text, logErrors,
        [](std::vector<std::string> const& fields, ActivityHuntTarget& target, std::string& why)
        {
            if (fields.size() != 3)
                why = "expected name:spawn:team";
            else if (!ParseUInt(fields[1], 0xFFFFFFFFu, target.spawnId) || !target.spawnId)
                why = "the spawn must be a creature guid (a number above 0)";
            else if (!ParseTeam(fields[2], false, target.team))
                why = "the team must be alliance or horde";
        });
}

// Raid.Targets: name:spawn:team[:rallyX:rallyY].
std::vector<ActivityRaidTarget> ParseRaidTargets(std::string const& text, bool logErrors)
{
    return ParseEntries<ActivityRaidTarget>(
        "Raid.Targets", text, logErrors,
        [](std::vector<std::string> const& fields, ActivityRaidTarget& target, std::string& why)
        {
            if (fields.size() != 3 && fields.size() != 5)
                why = "expected name:spawn:team or name:spawn:team:rallyX:rallyY";
            else if (!ParseUInt(fields[1], 0xFFFFFFFFu, target.spawnId) || !target.spawnId)
                why = "the spawn must be a creature guid (a number above 0)";
            else if (!ParseTeam(fields[2], false, target.team))
                why = "the team must be alliance or horde (the attackers)";
            else if (fields.size() == 5)
            {
                if (!ParseFloat(fields[3], target.rallyX) || !ParseFloat(fields[4], target.rallyY))
                    why = "rallyX and rallyY must be decimal numbers";
                else
                    target.rallySet = true;
            }
        });
}

// Party.Spots, Loiter.Spots: name:team:map:x:y:z.
std::vector<ActivitySpot> ParsePlaces(char const* key, std::string const& text, bool logErrors)
{
    return ParseEntries<ActivitySpot>(
        key, text, logErrors,
        [](std::vector<std::string> const& fields, ActivitySpot& spot, std::string& why)
        {
            if (fields.size() != 6)
                why = "expected name:team:map:x:y:z";
            else if (!ParseTeam(fields[1], true, spot.team))
                why = "the team must be alliance, horde or any";
            else if (!ParseUInt(fields[2], 0xFFFFFFFFu, spot.mapId) || !ParseFloat(fields[3], spot.x) ||
                     !ParseFloat(fields[4], spot.y) || !ParseFloat(fields[5], spot.z))
                why = "the map must be a number and x, y, z decimal numbers";
        });
}

std::vector<ActivityCampaignTarget> ParseCampaignTargets(char const* key, std::string const& text, bool logErrors)
{
    return ParseEntries<ActivityCampaignTarget>(key, text, logErrors,
        [](std::vector<std::string> const& fields, ActivityCampaignTarget& target, std::string& why)
        {
            if (fields.size() != 11)
                why = "expected name:team:spawn:map:x:y:z:guardSpawnCSV:waveEntry:strategy:raidMap";
            else if (!ParseTeam(fields[1], false, target.team) ||
                     !ParseUInt(fields[2], 0xFFFFFFFFu, target.spawnId) || !target.spawnId ||
                     !ParseUInt(fields[3], 0xFFFFFFFFu, target.rally.mapId) ||
                     !ParseFloat(fields[4], target.rally.x) || !ParseFloat(fields[5], target.rally.y) ||
                     !ParseFloat(fields[6], target.rally.z) ||
                     !ParseUInt(fields[8], 0xFFFFFFFFu, target.waveEntry) ||
                     !ParseUInt(fields[10], 0xFFFFFFFFu, target.raidMap))
                why = "invalid faction, spawn, rally, wave entry or raid map";
            else
            {
                target.rally.set = true;
                target.strategy = Lower(fields[9]);
                if (!target.strategy.empty() && !ValidSpotName(target.strategy))
                    why = "invalid strategy name";
                for (std::string const& word : Split(fields[7], ','))
                {
                    if (word.empty())
                        continue;
                    uint32 guard = 0;
                    if (!ParseUInt(word, 0xFFFFFFFFu, guard) || !guard || target.guardSpawns.size() >= 40 ||
                        std::find(target.guardSpawns.begin(), target.guardSpawns.end(), guard) != target.guardSpawns.end() ||
                        guard == target.spawnId)
                    {
                        why = "invalid, duplicate or excessive guard spawn";
                        break;
                    }
                    target.guardSpawns.push_back(guard);
                }
            }
        });
}

std::vector<ActivityFestivalTarget> ParseFestivalTargets(std::string const& text, bool logErrors)
{
    return ParseEntries<ActivityFestivalTarget>("Festival.Targets", text, logErrors,
        [](std::vector<std::string> const& fields, ActivityFestivalTarget& target, std::string& why)
        {
            if (fields.size() != 5)
                why = "expected name:event:interaction:spawn:team";
            else if (!ParseUInt(fields[1], 65535, target.eventId) || !target.eventId ||
                     !ParseUInt(fields[3], 0xFFFFFFFFu, target.spawnId) || !target.spawnId ||
                     !ParseTeam(fields[4], true, target.team))
                why = "invalid event, spawn or faction";
            else
            {
                target.interaction = Lower(fields[2]);
                if (target.interaction != "faire" && target.interaction != "ribbon" &&
                    target.interaction != "bonfire" && target.interaction != "fishing")
                    why = "interaction must be faire, ribbon, bonfire or fishing";
            }
        });
}

// A comma list of words, trimmed and ASCII-lowered, empties dropped.
std::vector<std::string> ParseWords(std::string const& text)
{
    std::vector<std::string> words;
    for (std::string const& word : Split(text, ','))
        if (!word.empty())
            words.push_back(Lower(word));
    return words;
}

// Dungeon.Ids: a comma list of LFG dungeon ids.
std::vector<uint32> ParseIds(std::string const& text)
{
    std::vector<uint32> ids;
    for (std::string const& word : Split(text, ','))
    {
        if (word.empty())
            continue;
        uint32 id = 0;
        if (!ParseUInt(word, 0xFFFFFFFFu, id) || !id)
        {
            LOG_ERROR("playerbots.activity",
                      "AiPlayerbot.CoaActivity.Dungeon.Ids: entry '{}' skipped (not a dungeon id)", word);
            continue;
        }
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    }
    return ids;
}

std::string Option(std::string const& key, char const* def)
{
    return sConfigMgr->GetOption<std::string>(Prefix + key, def);
}

template <typename Entry>
Entry const* FindNamed(std::vector<Entry> const& entries, std::string const& name)
{
    for (Entry const& entry : entries)
        if (StringEqualI(entry.name, name))
            return &entry;
    return nullptr;
}
}  // namespace

bool ActivityConfig::KindEnabled(ActivityKindId kind) const
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return duelEnable;
        case ActivityKindId::Gang:
            return gangEnable;
        case ActivityKindId::Dungeon:
            return dungeonEnable;
        case ActivityKindId::Hunt:
            return huntEnable;
        case ActivityKindId::Raid:
            return raidEnable;
        case ActivityKindId::Party:
            return partyEnable;
        case ActivityKindId::Loiter:
            return loiterEnable;
        case ActivityKindId::Gurubashi:
            return gurubashiEnable;
        case ActivityKindId::Escort:
            return escortEnable;
        case ActivityKindId::RareScout:
            return rareScoutEnable;
        case ActivityKindId::LeaderEncounter:
            return leaderEnable;
        case ActivityKindId::Frontline:
            return frontlineEnable;
        case ActivityKindId::TownDefense:
            return townDefenseEnable;
        case ActivityKindId::PugRaid:
            return pugRaidEnable;
        case ActivityKindId::WorldBoss:
            return worldBossEnable;
        case ActivityKindId::PlayerService:
            return serviceEnable;
        case ActivityKindId::FestivalVisit:
            return festivalEnable;
        case ActivityKindId::GoldRoll:
            return goldRollEnable;
        case ActivityKindId::PvpGang:
            return pvpGangEnable;
        case ActivityKindId::Bounty:
            return bountyEnable;
        case ActivityKindId::RareCamp:
            return rareCampEnable;
        case ActivityKindId::HubDefense:
            return hubDefenseEnable;
        case ActivityKindId::CapitalRaid:
            return capitalEnable;
        case ActivityKindId::LieutenantRaid:
            return lieutenantEnable;
        default:
            return false;
    }
}

uint32 ActivityConfig::KindWeight(ActivityKindId kind) const
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return weightDuel;
        case ActivityKindId::Gang:
            return weightGang;
        case ActivityKindId::Dungeon:
            return weightDungeon;
        case ActivityKindId::Hunt:
            return weightHunt;
        case ActivityKindId::Raid:
            return weightRaid;
        case ActivityKindId::Party:
            return weightParty;
        case ActivityKindId::Loiter:
            return weightLoiter;
        case ActivityKindId::Gurubashi:
            return weightGurubashi;
        case ActivityKindId::Escort:
            return 0;  // explicit player request only, regardless of config or brain
        case ActivityKindId::RareScout:
            return weightRareScout;
        case ActivityKindId::LeaderEncounter:
            return 0;
        case ActivityKindId::Frontline:
            return weightFrontline;
        case ActivityKindId::TownDefense:
            return weightTownDefense;
        case ActivityKindId::PugRaid:
            return weightPugRaid;
        case ActivityKindId::WorldBoss:
            return weightWorldBoss;
        case ActivityKindId::PlayerService:
            return 0;
        case ActivityKindId::FestivalVisit:
            return weightFestival;
        case ActivityKindId::GoldRoll:
            return 0;
        case ActivityKindId::PvpGang:
            return weightPvpGang;
        case ActivityKindId::Bounty:
            return 0;
        case ActivityKindId::RareCamp:
            return weightRareCamp;
        case ActivityKindId::HubDefense:
            return 0;
        case ActivityKindId::CapitalRaid:
            return weightCapital;
        case ActivityKindId::LieutenantRaid:
            return weightLieutenant;
        default:
            return 0;
    }
}

uint32 ActivityConfig::KindMax(ActivityKindId kind) const
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return duelMax;
        case ActivityKindId::Gang:
            return gangMax;
        case ActivityKindId::Dungeon:
            return dungeonMax;
        case ActivityKindId::Hunt:
            return huntMax;
        case ActivityKindId::Raid:
            return raidMax;
        case ActivityKindId::Party:
            return partyMax;
        case ActivityKindId::Loiter:
            return loiterMax;
        case ActivityKindId::Gurubashi:
            return 1;
        case ActivityKindId::Escort:
            return escortMax;
        case ActivityKindId::RareScout:
            return rareScoutMax;
        case ActivityKindId::LeaderEncounter:
            return leaderMax;
        case ActivityKindId::Frontline:
            return frontlineMax;
        case ActivityKindId::TownDefense:
            return townDefenseMax;
        case ActivityKindId::PugRaid:
            return pugRaidMax;
        case ActivityKindId::WorldBoss:
            return worldBossMax;
        case ActivityKindId::PlayerService:
            return serviceMax;
        case ActivityKindId::FestivalVisit:
            return festivalMax;
        case ActivityKindId::GoldRoll:
            return goldRollMax;
        case ActivityKindId::PvpGang:
            return pvpGangMax;
        case ActivityKindId::Bounty:
            return bountyMax;
        case ActivityKindId::RareCamp:
            return rareCampMax;
        case ActivityKindId::HubDefense:
            return hubDefenseMax;
        case ActivityKindId::CapitalRaid:
            return capitalMax;
        case ActivityKindId::LieutenantRaid:
            return lieutenantMax;
        default:
            return 0;
    }
}

uint32 ActivityConfig::KindMinBots(ActivityKindId kind) const
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return duelMinBots;
        case ActivityKindId::Gang:
            return gangMinBots;
        case ActivityKindId::Dungeon:
            return dungeonBots;
        case ActivityKindId::Hunt:
            return huntMinBots;
        case ActivityKindId::Raid:
            return raidMinBots;
        case ActivityKindId::Party:
            return partyMinBots;
        case ActivityKindId::Loiter:
            return loiterMinBots;
        case ActivityKindId::Gurubashi:
            return gurubashiMinBots;
        case ActivityKindId::Escort:
        case ActivityKindId::RareScout:
            return 1;
        case ActivityKindId::LeaderEncounter:
            return leaderMinBots;
        case ActivityKindId::Frontline:
            return frontlineMinBots;
        case ActivityKindId::TownDefense:
            return townDefenseMinBots;
        case ActivityKindId::PugRaid:
            return pugRaidMinBots;
        case ActivityKindId::WorldBoss:
            return worldBossMinBots;
        case ActivityKindId::PlayerService:
            return 1;
        case ActivityKindId::FestivalVisit:
            return 1;
        case ActivityKindId::GoldRoll:
            return 1;
        case ActivityKindId::PvpGang:
            return pvpGangMinBots;
        case ActivityKindId::Bounty:
            return bountyMinBots;
        case ActivityKindId::RareCamp:
            return rareCampMinBots;
        case ActivityKindId::HubDefense:
            return frontlineMinBots;
        case ActivityKindId::CapitalRaid:
            return capitalMinBots;
        case ActivityKindId::LieutenantRaid:
            return lieutenantMinBots;
        default:
            return 0;
    }
}

ActivityDuelSpot const* ActivityConfig::FindSpot(std::string const& name) const
{
    for (ActivityDuelSpot const& spot : duelSpots)
        if (StringEqualI(spot.name, name))
            return &spot;
    return nullptr;
}

char const* ActivityConfig::DefaultDuelSpots()
{
    return "orgrimmar_gate:horde:1:1380.0:-4390.0:27.0:10:60;ironforge_gate:alliance:0:-5025.0:-820.0:495.0:10:60;"
           "razor_hill:horde:1:325.0:-4705.0:14.0:1:12;goldshire:alliance:0:-9456.0:72.0:56.0:1:12";
}

ActivityHuntTarget const* ActivityConfig::FindHuntTarget(std::string const& name) const
{
    return FindNamed(huntTargets, name);
}

ActivityRaidTarget const* ActivityConfig::FindRaidTarget(std::string const& name) const
{
    return FindNamed(raidTargets, name);
}

ActivitySpot const* ActivityConfig::FindPartySpot(std::string const& name) const
{
    return FindNamed(partySpots, name);
}

ActivitySpot const* ActivityConfig::FindLoiterSpot(std::string const& name) const
{
    return FindNamed(loiterSpots, name);
}

ActivitySpot const* ActivityConfig::FindEscortSpot(std::string const& name) const
{
    return FindNamed(escortSpots, name);
}

ActivityHuntTarget const* ActivityConfig::FindRareScoutTarget(std::string const& name) const
{
    return FindNamed(rareScoutTargets, name);
}

ActivityCampaignTarget const* ActivityConfig::FindLeaderTarget(std::string const& name) const
{
    return FindNamed(leaderTargets, name);
}

ActivityCampaignTarget const* ActivityConfig::FindFrontlineTarget(std::string const& name) const
{
    return FindNamed(frontlineTargets, name);
}

ActivityCampaignTarget const* ActivityConfig::FindTownDefenseTarget(std::string const& name) const
{
    return FindNamed(townDefenseTargets, name);
}

ActivityCampaignTarget const* ActivityConfig::FindPugRaidTarget(std::string const& name) const
{
    return FindNamed(pugRaidTargets, name);
}

ActivityCampaignTarget const* ActivityConfig::FindWorldBossTarget(std::string const& name) const
{
    return FindNamed(worldBossTargets, name);
}

ActivityFestivalTarget const* ActivityConfig::FindFestivalTarget(std::string const& name) const
{
    return FindNamed(festivalTargets, name);
}

ActivitySpot const* ActivityConfig::FindPvpGangSpot(std::string const& name) const
{
    return FindNamed(pvpGangSpots, name);
}

ActivityHuntTarget const* ActivityConfig::FindRareCampTarget(std::string const& name) const
{
    return FindNamed(rareCampTargets, name);
}

std::vector<std::string> ActivityConfig::SelfTest()
{
    uint32 passed = 0;
    uint32 failed = 0;
    std::vector<std::string> lines;
    auto const check = [&](bool ok, char const* name)
    {
        if (ok)
            ++passed;
        else
        {
            ++failed;
            lines.push_back(std::string("FAIL M7 config/request: ") + name);
        }
    };
    check(uint8(ActivityKindId::Duel) == 1 && uint8(ActivityKindId::Gang) == 2 &&
              uint8(ActivityKindId::Dungeon) == 3 && uint8(ActivityKindId::Hunt) == 4 &&
              uint8(ActivityKindId::Raid) == 5 && uint8(ActivityKindId::Party) == 6 &&
              uint8(ActivityKindId::Loiter) == 7 && uint8(ActivityKindId::Gurubashi) == 8 &&
              uint8(ActivityKindId::Escort) == 9 && uint8(ActivityKindId::RareScout) == 10,
          "append-only kind ids");
    for (uint8 k = 1; k < uint8(ActivityKindId::Count); ++k)
    {
        auto const parsed = ActivityKindFromName(ActivityKindName(ActivityKindId(k)));
        check(parsed && uint8(*parsed) == k, "kind name round trip");
    }
    check(ActivityKindFromName("ESCORT") == ActivityKindId::Escort &&
              ActivityKindFromName("RareScout") == ActivityKindId::RareScout &&
              !ActivityKindFromName("none") && !ActivityKindFromName("unknown"),
          "case insensitive names, rare alias and invalid names");
    check(ActivityKindIsGroup(ActivityKindId::Escort) && !ActivityKindIsGroup(ActivityKindId::RareScout) &&
              ActivityKindIsGroup(ActivityKindId::Gang) && !ActivityKindIsGroup(ActivityKindId::Party),
          "group and solo kind dispatch");
    ActivityConfig config;
    for (uint8 k = uint8(ActivityKindId::LeaderEncounter); k < uint8(ActivityKindId::Count); ++k)
        check(!config.KindEnabled(ActivityKindId(k)) && config.KindWeight(ActivityKindId(k)) == 0,
              "advanced activities default off without scheduling weights");
    check(config.pvpGangHighRiskPercent == 50 && !config.pvpGangGmVerified && !config.leaderSpikeVerified,
          "mixed per-group rulesets and unverified runtime gates");
    auto const campaign = ParseCampaignTargets("Frontline.Targets",
        "mill:alliance:123:0:1:2:3:456,789:0::0", false);
    check(campaign.size() == 1 && campaign.front().rally.set && campaign.front().guardSpawns.size() == 2 &&
              campaign.front().strategy.empty(), "campaign exact fields with optional guards/strategy");
    check(ParseCampaignTargets("Leader.Targets", "x:horde:0:1:0:0:0::0::0", false).empty() &&
          ParseCampaignTargets("Leader.Targets", "x:horde:1:1:nan:0:0::0::0", false).empty() &&
          ParseCampaignTargets("Leader.Targets", "x:horde:1:1:0:0:0:2,2:0::0", false).empty() &&
          ParseCampaignTargets("Leader.Targets", "x:horde:1:1:0:0:0:1:0::0", false).empty() &&
          ParseCampaignTargets("Leader.Targets", "x:horde:1:1:0:0:0:4294967296:0::0", false).empty(),
          "campaign zero spawn/nonfinite/duplicate/self/overflow guards rejected");
    check(ParseCampaignTargets("Leader.Targets", "", false).empty(), "no implicit capital/leader targets");
    auto const festivals = ParseFestivalTargets("faire:any:faire:123:any;fire:1:bonfire:124:alliance", false);
    check(festivals.size() == 1 && festivals.front().interaction == "bonfire",
          "festival event numeric and native bonfire mode");
    check(ParseFestivalTargets("fishing:15:fishing:125:any;faire:4:faire:126:any;ribbon:1:ribbon:127:horde", false).size() == 3,
          "fishing/faire/ribbon exact whitelist modes");
    check(ParseFestivalTargets("x:1:quest_reward:2:alliance", false).empty() &&
          ParseFestivalTargets("x:0:faire:2:alliance", false).empty(), "unsupported festival mode/event rejected");
    config.escortEnable = true;
    check(config.KindWeight(ActivityKindId::Escort) == 0 && config.KindWeight(ActivityKindId::RareScout) == 0 &&
              config.KindMinBots(ActivityKindId::Escort) == 1 && config.KindMinBots(ActivityKindId::RareScout) == 1,
          "explicit escort scheduling and single-bot minimums");
    check(!config.rareScoutEnable && config.KindWeight(ActivityKindId::Duel) == 30 &&
              config.KindWeight(ActivityKindId::Gang) == 20 && config.KindWeight(ActivityKindId::Dungeon) == 30 &&
              config.KindWeight(ActivityKindId::Hunt) == 15 && config.KindWeight(ActivityKindId::Raid) == 5 &&
              config.KindWeight(ActivityKindId::Party) == 10 && config.KindWeight(ActivityKindId::Loiter) == 10 &&
              config.KindWeight(ActivityKindId::Gurubashi) == 20,
          "legacy weights unchanged");
    config.escortSpots = ParsePlaces("Escort.Spots",
                                     "Goldshire:any:0:-9456:72:56;razor_hill:horde:1:325:-4705:14", false);
    check(config.escortSpots.size() == 2 && config.FindEscortSpot("GOLDSHIRE") &&
              config.FindEscortSpot("goldshire")->team == 2 && !config.FindEscortSpot("missing"),
          "configured destination parsing and lookup");
    check(ParsePlaces("Escort.Spots", "", false).empty(), "no implicit escort destinations");
    check(ParsePlaces("Escort.Spots", "spot:wrong:0:1:2:3;spot2:any:-1:1:2:3;spot3:any:0:nan:2:3;"
                                      "spot4:any:0:1:inf:3;spot5:any:0:1:2:3x;spot6:any:0:1:2", false).empty(),
          "reject invalid destination team, map, floats and fields");
    check(ParsePlaces("Escort.Spots", "town:alliance:0:1:2:3;TOWN:horde:1:4:5:6", false).size() == 1,
          "duplicate destination names are folded and rejected");
    config.rareScoutTargets = ParseHuntTargets("narg:80647:alliance;thuros:81107:alliance", false,
                                               "RareScout.Targets");
    check(config.rareScoutTargets.size() == 2 && config.FindRareScoutTarget("NARG") &&
              config.FindRareScoutTarget("narg")->spawnId == 80647 && !config.FindRareScoutTarget("missing"),
          "rare target parsing and lookup");
    check(ParseHuntTargets("", false, "RareScout.Targets").empty(), "no implicit rare spawns");
    check(ParseHuntTargets("a:0:alliance;b:-1:horde;c:4294967296:alliance;d:12:any;e:12:alliance:extra", false,
                            "RareScout.Targets").empty(),
          "reject zero, negative, oversized and malformed rare targets");
    check(ParseHuntTargets("narg:80647:alliance;NARG:81107:alliance", false, "RareScout.Targets").size() == 1,
          "duplicate rare target names are rejected");
    check(ActivityRequestRules::OwnsEscort(ActivityKindId::Escort, 42, 42) &&
              !ActivityRequestRules::OwnsEscort(ActivityKindId::Escort, 42, 43) &&
              !ActivityRequestRules::OwnsEscort(ActivityKindId::Escort, 0, 0) &&
              !ActivityRequestRules::OwnsEscort(ActivityKindId::Hunt, 42, 42),
          "cancellation belongs only to the nonempty requesting GUID");
    check(ActivityRequestRules::RequestThrottled(1000, 1000) &&
              ActivityRequestRules::RequestThrottled(1000, 5999) &&
              !ActivityRequestRules::RequestThrottled(1000, 6000) &&
              ActivityRequestRules::RequestThrottled(1000, 999) &&
              ActivityRequestRules::RequestThrottled(UINT64_MAX - 300, UINT64_MAX),
          "request gap boundary, clock rewind and addition overflow");
    check(ActivityRequestRules::SameRareTarget("narg", 0, "narg", 0) &&
              !ActivityRequestRules::SameRareTarget("narg", 0, "thuros", 0) &&
              !ActivityRequestRules::SameRareTarget("narg", 0, "narg", 1) &&
              !ActivityRequestRules::SameRareTarget("", 0, "", 0) &&
              ActivityRequestRules::SameRareTarget("narg", 0, "alias", 1, 80647, 80647) &&
              !ActivityRequestRules::SameRareTarget("narg", 0, "alias", 1, 80647, 81107),
          "target exclusivity covers canonical name/team and aliases of one spawn");
    lines.insert(lines.begin(), "M7 config/request self-test: " + std::to_string(passed) + " passed, " +
                                   std::to_string(failed) + " failed");
    return lines;
}

char const* ActivityConfig::DefaultHuntTargets() { return "hogger:80531:alliance;gallywix:14412:horde"; }

char const* ActivityConfig::DefaultRaidTargets()
{
    return "maleb:15322:horde:-642:-594;darthalia:15542:alliance:-253:-808";
}

char const* ActivityConfig::DefaultLoiterSpots()
{
    return "stormwind_bank:alliance:0:-8935.3:613.1:99.6;ironforge_bank:alliance:0:-4886.5:-997.6:504.0;"
           "darnassus_bank:alliance:1:9942.0:2519.2:1317.7;orgrimmar_bank:horde:1:1627.3:-4376.1:12.1;"
           "undercity_bank:horde:0:1595.9:240.3:-52.1;thunderbluff_bank:horde:1:-1257.6:22.8:128.3";
}

std::shared_ptr<ActivityConfig const> ActivityConfig::Load()
{
    auto config = std::make_shared<ActivityConfig>();
    config->enable = Bool("Enable", false);

    // Activities off: no other key is read, so a conf that does not list them logs nothing new.
    if (!config->enable)
        return config;

    config->tickMs = Int("TickMs", 1000, 100, 10000);
    config->scheduleSeconds = Int("ScheduleSeconds", 600, 30, 86400);
    config->maxConcurrent = Int("MaxConcurrent", 2, 0, 20);
    config->maxParticipants = Int("MaxParticipants", 60, 0, 1000);
    config->maxParticipantsPercent = Int("MaxParticipantsPercent", 20, 0, 100);
    config->suspendAboveMs = Int("SuspendAboveMs", 120, 10, 10000);
    config->abortAboveMs = Int("AbortAboveMs", 200, int32(config->suspendAboveMs), 10000);
    config->maxOpsPerTick = Int("MaxOpsPerTick", 20, 1, 1000);
    config->botCooldownMinutes = Int("BotCooldownMinutes", 90, 0, 1440);
    config->acceptPercent = Int("AcceptPercent", 70, 0, 100);
    config->requireRealPlayerOnline = Bool("RequireRealPlayerOnline", true);
    config->ambientNearPlayerBrackets = Int("AmbientNearPlayerBrackets", 1, 0, 5);
    config->reviveAfterSeconds = Int("Revive.AfterSeconds", 45, 5, 600);
    config->brainDeadlineMs = Int("BrainDeadlineMs", 1500, 200, 10000);
    config->logKeepDays = Int("LogKeepDays", 30, 1, 3650);

    config->duelEnable = Bool("Duel.Enable", true);
    config->weightDuel = Int("Weight.Duel", 30, 0, 1000);
    config->duelSpots = ParseSpots(sConfigMgr->GetOption<std::string>(Prefix + "Duel.Spots", ""), true);
    if (config->duelSpots.empty())
        config->duelSpots = ParseSpots(DefaultDuelSpots(), false);
    config->duelMinBots = Int("Duel.MinBots", 4, 2, 20);
    config->duelMaxBots = Int("Duel.MaxBots", 8, int32(config->duelMinBots), 20);
    config->duelMax = Int("Duel.Max", 1, 0, 4);
    config->duelMinutes = Int("Duel.Minutes", 30, 5, 240);
    config->duelCallBoardMinHp = Int("Duel.CallBoardMinHp", 60, 1, 100);
    config->duelPairCooldownMinutes = Int("Duel.PairCooldownMinutes", 10, 0, 1440);
    config->duelPlayerCooldownMinutes = Int("Duel.PlayerCooldownMinutes", 10, 0, 1440);
    config->duelMaxAcceptsPerPlayerPerDay = Int("Duel.MaxAcceptsPerPlayerPerDay", 3, 0, 100);
    config->duelPreferCallBoardHolders = Bool("Duel.PreferCallBoardHolders", false);
    config->duelAmbient = Bool("Duel.Ambient", false);

    config->gangEnable = Bool("Gang.Enable", true);
    config->weightGang = Int("Weight.Gang", 20, 0, 1000);
    config->gangMinBots = Int("Gang.MinBots", 3, 2, 10);
    config->gangMaxBots = Int("Gang.MaxBots", 5, int32(config->gangMinBots), 10);
    config->gangMax = Int("Gang.Max", 2, 0, 10);
    config->gangMinutes = Int("Gang.Minutes", 45, 10, 240);
    config->gangLevelSpread = Int("Gang.LevelSpread", 3, 0, 10);

    std::string const ruleset = Lower(Trimmed(sConfigMgr->GetOption<std::string>(Prefix + "PvpRuleset", "keep")));
    if (ruleset == "warmode")
        config->pvpWarMode = true;
    else if (ruleset != "keep")
        LOG_ERROR("playerbots.activity", "AiPlayerbot.CoaActivity.PvpRuleset = '{}' is not keep or warmode: keep",
                  ruleset);

    config->travelWalkMaxYards = Int("Travel.WalkMaxYards", 700, 50, 5000);
    config->travelStragglerYards = Int("Travel.StragglerYards", 150, 30, 2000);
    config->travelStragglerSeconds = Int("Travel.StragglerSeconds", 45, 5, 600);
    config->travelRallyWaitSeconds = Int("Travel.RallyWaitSeconds", 300, 0, 3600);
    config->travelGatherTimeoutSeconds = Int("Travel.GatherTimeoutSeconds", 600, 30, 3600);

    std::string const channel = Lower(Trimmed(sConfigMgr->GetOption<std::string>(Prefix + "Chat.Channel", "zone")));
    if (channel == "world")
        config->chatChannel = ActivityChatChannel::World;
    else if (channel == "none")
        config->chatChannel = ActivityChatChannel::None;
    else
    {
        config->chatChannel = ActivityChatChannel::Zone;
        if (channel != "zone")
            LOG_ERROR("playerbots.activity", "AiPlayerbot.CoaActivity.Chat.Channel = '{}' is not zone, world or none: "
                      "zone", channel);
    }
    config->chatMaxPosts = Int("Chat.MaxPosts", 4, 0, 50);
    config->chatMinGapSeconds = Int("Chat.MinGapSeconds", 60, 0, 3600);

    // M5 chat recruitment.
    config->chatChannels = ParseWords(Option("Chat.Channels", "Zone"));
    if (config->chatChannels.empty())
    {
        LOG_ERROR("playerbots.activity", "AiPlayerbot.CoaActivity.Chat.Channels is empty: zone");
        config->chatChannels = {"zone"};
    }
    config->chatRecruitEverySeconds = Int("Chat.RecruitEverySeconds", 180, 30, 3600);
    config->chatJoinWords = ParseWords(Option("Chat.JoinWords", "inv,x,moi"));
    config->whisperPrefix = Lower(Trimmed(Option("WhisperPrefix", "!")));
    if (config->whisperPrefix.size() > 4)
    {
        LOG_ERROR("playerbots.activity", "AiPlayerbot.CoaActivity.WhisperPrefix = '{}' is longer than 4 bytes: !",
                  config->whisperPrefix);
        config->whisperPrefix = "!";
    }

    // M5 dungeon runs.
    config->dungeonEnable = Bool("Dungeon.Enable", true);
    config->weightDungeon = Int("Weight.Dungeon", 30, 0, 1000);
    config->dungeonBots = Int("Dungeon.Bots", 4, 3, 4);
    config->dungeonLevelSpread = Int("Dungeon.LevelSpread", 3, 0, 5);
    config->dungeonMax = Int("Dungeon.Max", 1, 0, 4);
    config->dungeonRecruitMinutes = Int("Dungeon.RecruitMinutes", 10, 2, 60);
    config->dungeonQueueMinutes = Int("Dungeon.QueueMinutes", 10, 2, 60);
    config->dungeonLfgTries = Int("Dungeon.LfgTries", 2, 1, 5);
    config->dungeonUseLfg = Bool("Dungeon.UseLfg", true);
    config->dungeonMaxMinutes = Int("Dungeon.MaxMinutes", 120, 30, 360);
    config->dungeonIdleDisbandMinutes = Int("Dungeon.IdleDisbandMinutes", 5, 1, 60);
    config->dungeonPlayerCooldownMinutes = Int("Dungeon.PlayerCooldownMinutes", 60, 0, 1440);
    config->dungeonSwitchSpecs = Bool("Dungeon.SwitchSpecs", true);
    config->dungeonIds = ParseIds(Option("Dungeon.Ids", ""));

    // M5 elite hunts.
    config->huntEnable = Bool("Hunt.Enable", true);
    config->weightHunt = Int("Weight.Hunt", 15, 0, 1000);
    config->huntTargets = ParseHuntTargets(Option("Hunt.Targets", ""), true);
    if (config->huntTargets.empty())
        config->huntTargets = ParseHuntTargets(DefaultHuntTargets(), false);
    config->huntMinBots = Int("Hunt.MinBots", 3, 2, 10);
    config->huntMaxBots = Int("Hunt.MaxBots", 6, int32(config->huntMinBots), 10);
    config->huntMax = Int("Hunt.Max", 1, 0, 4);
    config->huntRecruitSeconds = Int("Hunt.RecruitSeconds", 120, 0, 1800);
    config->huntMinutes = Int("Hunt.Minutes", 20, 5, 120);
    config->huntLevelBelow = Int("Hunt.LevelBelow", 2, 0, 10);
    config->huntLevelAbove = Int("Hunt.LevelAbove", 3, 0, 10);

    // M5 outpost raids. MaxBots stops at 39 (a player takes the 40th place), so MinBots does too.
    config->raidEnable = Bool("Raid.Enable", false);
    config->weightRaid = Int("Weight.Raid", 5, 0, 1000);
    config->raidTargets = ParseRaidTargets(Option("Raid.Targets", ""), true);
    if (config->raidTargets.empty())
        config->raidTargets = ParseRaidTargets(DefaultRaidTargets(), false);
    config->raidMinLevel = Int("Raid.MinLevel", 45, 1, 80);
    config->raidMinBots = Int("Raid.MinBots", 6, 2, 40);
    config->raidMaxBots = Int("Raid.MaxBots", 15, int32(std::min<uint32>(config->raidMinBots, 39)), 39);
    config->raidMinBots = std::min(config->raidMinBots, config->raidMaxBots);
    config->raidMax = Int("Raid.Max", 1, 0, 2);
    config->raidRecruitMinutes = Int("Raid.RecruitMinutes", 10, 1, 60);
    config->raidTimeoutMinutes = Int("Raid.TimeoutMinutes", 25, 5, 180);
    config->raidQuestGiverGuardYards = Int("Raid.QuestGiverGuardYards", 100, 0, 500);
    config->raidMaxPerTargetPerDay = Int("Raid.MaxPerTargetPerDay", 2, 0, 24);
    config->raidSkipIfDefenderInZone = Bool("Raid.SkipIfDefenderInZone", true);
    std::string const loot = Lower(Trimmed(Option("Raid.LootMethod", "group")));
    static char const* const LootMethods[] = {"free", "roundrobin", "master", "group", "need"};
    auto const method = std::find(std::begin(LootMethods), std::end(LootMethods), loot);
    if (method != std::end(LootMethods))
        config->raidLootMethod = uint8(method - std::begin(LootMethods));
    else
    {
        config->raidLootMethod = 3;
        LOG_ERROR("playerbots.activity", "AiPlayerbot.CoaActivity.Raid.LootMethod = '{}' is not free, roundrobin, "
                  "master, group or need: group", loot);
    }
    config->raidLootThreshold = uint8(Int("Raid.LootThreshold", 2, 0, 6));

    // M5 inn parties.
    config->partyEnable = Bool("Party.Enable", true);
    config->weightParty = Int("Weight.Party", 10, 0, 1000);
    config->partyMinBots = Int("Party.MinBots", 4, 2, 12);
    config->partyMaxBots = Int("Party.MaxBots", 8, int32(config->partyMinBots), 12);
    config->partyMax = Int("Party.Max", 1, 0, 4);
    config->partyMinutes = Int("Party.Minutes", 15, 5, 120);
    config->partyLevelSpread = Int("Party.LevelSpread", 5, 0, 20);
    config->partyDancePercent = Int("Party.DancePercent", 30, 0, 100);
    config->partyMaxYards = Int("Party.MaxYards", 2000, 100, 10000);
    config->partySpots = ParsePlaces("Party.Spots", Option("Party.Spots", ""), true);

    // M5 city loitering.
    config->loiterEnable = Bool("Loiter.Enable", true);
    config->weightLoiter = Int("Weight.Loiter", 10, 0, 1000);
    config->loiterMinBots = Int("Loiter.MinBots", 3, 2, int32(ActivityThreshold::LOITER_MAX_BOTS));
    config->loiterMaxBots =
        Int("Loiter.MaxBots", 8, int32(config->loiterMinBots), int32(ActivityThreshold::LOITER_MAX_BOTS));
    config->loiterMax = Int("Loiter.Max", 2, 0, 6);
    config->loiterMinutes = Int("Loiter.Minutes", 30, 5, 240);
    config->loiterMinLevel = Int("Loiter.MinLevel", 20, 1, 80);
    config->loiterSpots = ParsePlaces("Loiter.Spots", Option("Loiter.Spots", ""), true);
    if (config->loiterSpots.empty())
        config->loiterSpots = ParsePlaces("Loiter.Spots", DefaultLoiterSpots(), false);

    // M5 Gurubashi booty run.
    config->gurubashiEnable = Bool("Gurubashi.Enable", true);
    config->weightGurubashi = Int("Weight.Gurubashi", 20, 0, 1000);
    config->gurubashiMinBots = Int("Gurubashi.MinBots", 4, 2, 8);
    config->gurubashiMaxBots = Int("Gurubashi.MaxBots", 8, int32(config->gurubashiMinBots), 8);
    config->gurubashiMinLevel = Int("Gurubashi.MinLevel", 35, 1, 80);
    config->gurubashiMinutes = Int("Gurubashi.Minutes", 25, 5, 120);
    config->gurubashiBotsMayWin = Bool("Gurubashi.BotsMayWin", false);

    // M7: opt-in kinds do not inherit destinations or spawns when their lists are empty or malformed.
    config->escortEnable = Bool("Escort.Enable", false);
    config->escortBots = Int("Escort.Bots", 2, 1, 4);
    config->escortMax = Int("Escort.Max", 2, 0, 4);
    config->escortMinutes = Int("Escort.Minutes", 15, 1, 30);
    config->escortInviteSeconds = Int("Escort.InviteSeconds", 60, 10, 120);
    config->escortPlayerCooldownMinutes = Int("Escort.PlayerCooldownMinutes", 60, 0, 1440);
    config->escortLevelSpread = Int("Escort.LevelSpread", 5, 0, 10);
    config->escortNearbyYards = Int("Escort.NearbyYards", 200, 20, 1000);
    config->escortArrivalYards = Int("Escort.ArrivalYards", 20, 5, 100);
    config->escortSpots = ParsePlaces("Escort.Spots", Option("Escort.Spots", ""), true);

    config->rareScoutEnable = Bool("RareScout.Enable", false);
    config->weightRareScout = Int("Weight.RareScout", 0, 0, 1000);
    config->rareScoutMax = Int("RareScout.Max", 1, 0, 4);
    config->rareScoutMinutes = Int("RareScout.Minutes", 15, 1, 60);
    config->rareScoutLevelSpread = Int("RareScout.LevelSpread", 5, 0, 10);
    config->rareScoutNearbyYards = Int("RareScout.NearbyYards", 500, 20, 2000);
    config->rareScoutTargets = ParseHuntTargets(Option("RareScout.Targets", ""), true, "RareScout.Targets");

    config->leaderSpikeVerified = Bool("Leader.SpikeVerified", false);
    config->pvpGangGmVerified = Bool("PvpGang.GmVerified", false);
    config->capitalEnable = Bool("Capital.Enable", false);
    config->weightCapital = Int("Capital.Weight", 0, 0, 1000);
    config->capitalMax = Int("Capital.Max", 1, 0, 1);
    config->capitalMinBots = Int("Capital.MinBots", 20, 20, 40);
    config->capitalMaxBots = std::max(config->capitalMinBots, Int("Capital.MaxBots", 40, 20, 40));
    config->capitalTargets = ParseCampaignTargets("Capital.Targets", Option("Capital.Targets", ""), true);
    config->lieutenantEnable = Bool("Lieutenant.Enable", false);
    config->weightLieutenant = Int("Lieutenant.Weight", 0, 0, 1000);
    config->lieutenantMax = Int("Lieutenant.Max", 1, 0, 1);
    config->lieutenantMinBots = Int("Lieutenant.MinBots", 10, 10, 40);
    config->lieutenantMaxBots = std::max(config->lieutenantMinBots, Int("Lieutenant.MaxBots", 20, 10, 40));
    config->lieutenantTargets = ParseCampaignTargets("Lieutenant.Targets", Option("Lieutenant.Targets", ""), true);

    // Advanced activities retain empty whitelists until explicitly configured.
    config->leaderEnable = Bool("Leader.Enable", false);
    config->leaderSideEffectsAccepted = Bool("Leader.SideEffectsAccepted", false);
    config->frontlineEnable = Bool("Frontline.Enable", false);
    config->townDefenseEnable = Bool("TownDefense.Enable", false);
    config->pugRaidEnable = Bool("PugRaid.Enable", false);
    config->worldBossEnable = Bool("WorldBoss.Enable", false);
    config->frontlineAttackVerified = Bool("Frontline.AttackVerified", false);
    config->frontlineDefenders = Bool("Frontline.Defenders", false);
    config->frontlinePvpDefendersEnable = Bool("Frontline.PvpDefendersEnable", false);
    config->hubDefenseEnable = Bool("HubDefense.Enable", false);
    config->worldBossGmKillVerified = Bool("WorldBoss.GmKillVerified", false);
    config->serviceEnable = Bool("Service.Enable", false);
    config->festivalEnable = Bool("Festival.Enable", false);
    config->goldRollEnable = Bool("GoldRoll.Enable", false);
    config->pvpGangEnable = Bool("PvpGang.Enable", false);
    config->bountyEnable = Bool("Bounty.Enable", false);
    config->rareCampEnable = Bool("RareCamp.Enable", false);
    config->leaderMax = Int("Leader.Max", 1, 0, 1);
    config->leaderMinBots = Int("Leader.MinBots", 20, 1, 40);
    config->leaderMaxBots = Int("Leader.MaxBots", 40, 1, 40);
    config->leaderMinLevel = Int("Leader.MinLevel", 60, 1, 60);
    config->leaderScaleLevel = Int("Leader.ScaleLevel", 60, 1, 60);
    config->leaderMinutes = Int("Leader.Minutes", 30, 1, 60);
    config->leaderGatherSeconds = Int("Leader.GatherSeconds", 180, 10, 600);
    config->leaderNearbyYards = Int("Leader.NearbyYards", 5000, 100, 20000);
    config->leaderGuardYards = Int("Leader.GuardYards", 120, 20, 500);
    config->leaderHealth = Int("Leader.Health", 250000, 1, 1000000000);
    config->leaderGuardHealth = Int("Leader.GuardHealth", 15000, 1, 1000000000);
    config->leaderDamagePercent = Int("Leader.DamagePercent", 10, 1, 100);
    config->leaderGuardDamagePercent = Int("Leader.GuardDamagePercent", 20, 1, 100);
    config->leaderArmorPercent = Int("Leader.ArmorPercent", 25, 1, 100);
    config->weightFrontline = Int("Frontline.Weight", 0, 0, 1000);
    config->frontlineMax = Int("Frontline.Max", 1, 0, 4);
    config->frontlineMinBots = Int("Frontline.MinBots", 6, 1, 40);
    config->frontlineMaxBots = Int("Frontline.MaxBots", 15, 1, 40);
    config->frontlineRecruitSeconds = Int("Frontline.RecruitSeconds", 120, 0, 1800);
    config->frontlineMinutes = Int("Frontline.Minutes", 60, 1, 180);
    config->frontlineCleanRunsGate = Int("Frontline.CleanRunsGate", 10, 0, 1000);
    config->weightTownDefense = Int("TownDefense.Weight", 0, 0, 1000);
    config->townDefenseMax = Int("TownDefense.Max", 1, 0, 4);
    config->townDefenseMinBots = Int("TownDefense.MinBots", 3, 1, 40);
    config->townDefenseMaxBots = Int("TownDefense.MaxBots", 5, 1, 40);
    config->townDefenseRecruitSeconds = Int("TownDefense.RecruitSeconds", 120, 0, 1800);
    config->townDefenseMinutes = Int("TownDefense.Minutes", 60, 1, 180);
    config->weightPugRaid = Int("PugRaid.Weight", 0, 0, 1000);
    config->pugRaidMax = Int("PugRaid.Max", 1, 0, 4);
    config->pugRaidMinBots = Int("PugRaid.MinBots", 10, 1, 40);
    config->pugRaidMaxBots = Int("PugRaid.MaxBots", 40, 1, 40);
    config->pugRaidRecruitSeconds = Int("PugRaid.RecruitSeconds", 120, 0, 1800);
    config->pugRaidMinutes = Int("PugRaid.Minutes", 60, 1, 180);
    config->pugRaidCleanRunsGate = Int("PugRaid.CleanRunsGate", 0, 0, 1000);
    config->weightWorldBoss = Int("WorldBoss.Weight", 0, 0, 1000);
    config->worldBossMax = Int("WorldBoss.Max", 1, 0, 4);
    config->worldBossMinBots = Int("WorldBoss.MinBots", 20, 1, 40);
    config->worldBossMaxBots = Int("WorldBoss.MaxBots", 40, 1, 40);
    config->worldBossRecruitSeconds = Int("WorldBoss.RecruitSeconds", 120, 0, 1800);
    config->worldBossMinutes = Int("WorldBoss.Minutes", 60, 1, 180);
    config->worldBossCleanRunsGate = Int("WorldBoss.CleanRunsGate", 1, 0, 1000);
    config->frontlineGuardRadius = Int("Frontline.GuardRadius", 150, 20, 500);
    config->frontlineCooldownMinutes = Int("Frontline.CooldownMinutes", 360, 0, 1440);
    config->frontlineWarningSeconds = Int("Frontline.WarningSeconds", 300, 0, 1800);
    config->hubDefenseMax = Int("HubDefense.Max", 1, 0, 4);
    config->townDefenseWaves = Int("TownDefense.Waves", 3, 1, 20);
    config->townDefenseWaveSize = Int("TownDefense.WaveSize", 3, 1, 10);
    config->townDefenseWaveGapSeconds = Int("TownDefense.WaveGapSeconds", 30, 5, 300);
    config->townDefenseCleanWavesGate = Int("TownDefense.CleanWavesGate", 20, 0, 1000);
    config->pugRaidReservePerPlayer = Int("PugRaid.ReservePerPlayer", 1, 1, 5);
    config->serviceMax = Int("Service.Max", 2, 0, 4);
    config->serviceMinutes = Int("Service.Minutes", 10, 1, 30);
    config->serviceInviteSeconds = Int("Service.InviteSeconds", 60, 10, 120);
    config->servicePlayerCooldownMinutes = Int("Service.PlayerCooldownMinutes", 30, 0, 1440);
    config->serviceNearbyYards = Int("Service.NearbyYards", 200, 20, 1000);
    config->weightFestival = Int("Festival.Weight", 0, 0, 1000);
    config->festivalMax = Int("Festival.Max", 1, 0, 4);
    config->festivalMinutes = Int("Festival.Minutes", 5, 1, 30);
    config->festivalNearbyYards = Int("Festival.NearbyYards", 500, 20, 2000);
    config->goldRollMax = Int("GoldRoll.Max", 2, 0, 4);
    config->goldRollMaxCopper = Int("GoldRoll.MaxCopper", 100000, 1, 1000000);
    config->goldRollPlayerCooldownMinutes = Int("GoldRoll.PlayerCooldownMinutes", 30, 0, 1440);
    config->pvpGangHighRiskPercent = Int("PvpGang.HighRiskPercent", 50, 0, 100);
    config->pvpGangMax = Int("PvpGang.Max", 1, 0, 4);
    config->pvpGangMinBots = Int("PvpGang.MinBots", 3, 1, 5);
    config->pvpGangMaxBots = Int("PvpGang.MaxBots", 5, 1, 5);
    config->pvpGangMinutes = Int("PvpGang.Minutes", 30, 1, 60);
    config->pvpGangNearbyYards = Int("PvpGang.NearbyYards", 500, 20, 2000);
    config->weightPvpGang = Int("PvpGang.Weight", 0, 0, 1000);
    config->bountyMax = Int("Bounty.Max", 1, 0, 4);
    config->bountyMinBots = Int("Bounty.MinBots", 2, 1, 5);
    config->bountyMaxBots = Int("Bounty.MaxBots", 5, 1, 5);
    config->bountyMinutes = Int("Bounty.Minutes", 10, 1, 60);
    config->bountyNearbyYards = Int("Bounty.NearbyYards", 500, 20, 2000);
    config->rareCampMax = Int("RareCamp.Max", 1, 0, 4);
    config->rareCampMinBots = Int("RareCamp.MinBots", 2, 1, 5);
    config->rareCampMaxBots = Int("RareCamp.MaxBots", 5, 1, 5);
    config->rareCampMinutes = Int("RareCamp.Minutes", 15, 1, 60);
    config->rareCampNearbyYards = Int("RareCamp.NearbyYards", 500, 20, 2000);
    config->weightRareCamp = Int("RareCamp.Weight", 0, 0, 1000);
    config->leaderTargets = ParseCampaignTargets("Leader.Targets", Option("Leader.Targets", ""), true);
    config->frontlineTargets = ParseCampaignTargets("Frontline.Targets", Option("Frontline.Targets", ""), true);
    config->townDefenseTargets = ParseCampaignTargets("TownDefense.Targets", Option("TownDefense.Targets", ""), true);
    config->pugRaidTargets = ParseCampaignTargets("PugRaid.Targets", Option("PugRaid.Targets", ""), true);
    config->worldBossTargets = ParseCampaignTargets("WorldBoss.Targets", Option("WorldBoss.Targets", ""), true);
    config->festivalTargets = ParseFestivalTargets(Option("Festival.Targets", ""), true);
    config->pvpGangSpots = ParsePlaces("PvpGang.Spots", Option("PvpGang.Spots", ""), true);
    config->rareCampTargets = ParseHuntTargets(Option("RareCamp.Targets", ""), true, "RareCamp.Targets");
    config->leaderMaxBots = std::max(config->leaderMinBots, config->leaderMaxBots);
    config->frontlineMaxBots = std::max(config->frontlineMinBots, config->frontlineMaxBots);
    config->townDefenseMaxBots = std::max(config->townDefenseMinBots, config->townDefenseMaxBots);
    config->pugRaidMaxBots = std::max(config->pugRaidMinBots, config->pugRaidMaxBots);
    config->worldBossMaxBots = std::max(config->worldBossMinBots, config->worldBossMaxBots);
    config->pvpGangMaxBots = std::max(config->pvpGangMinBots, config->pvpGangMaxBots);
    config->bountyMaxBots = std::max(config->bountyMinBots, config->bountyMaxBots);
    config->rareCampMaxBots = std::max(config->rareCampMinBots, config->rareCampMaxBots);

    config->useBrainDuel = sConfigMgr->GetOption<bool>("AiPlayerbot.CoaBrain.Use.Duel", false);
    config->useBrainOrganizer = sConfigMgr->GetOption<bool>("AiPlayerbot.CoaBrain.Use.Organizer", false);
    config->brainDuelDeadlineMs =
        uint32(std::clamp(sConfigMgr->GetOption<int32>("AiPlayerbot.CoaBrain.Duel.DeadlineMs", 3000), 200, 10000));
    float const threshold = sConfigMgr->GetOption<float>("AiPlayerbot.CoaBrain.Duel.AcceptThreshold", 0.5f);
    config->brainDuelAcceptThreshold = std::isfinite(threshold) ? std::clamp(threshold, 0.0f, 1.0f) : 0.5f;
    return config;
}

void ActivityConfig::Publish(std::shared_ptr<ActivityConfig const> config)
{
    if (!config)
        config = std::make_shared<ActivityConfig const>();
    bool const enabled = config->enable;
    {
        std::lock_guard<std::mutex> guard(SnapshotLock());
        Snapshot() = std::move(config);
    }
    enabledFlag.store(enabled, std::memory_order_relaxed);
}

std::shared_ptr<ActivityConfig const> ActivityConfig::Current()
{
    std::lock_guard<std::mutex> guard(SnapshotLock());
    return Snapshot();
}

bool ActivityConfig::Enabled() { return enabledFlag.load(std::memory_order_relaxed); }
