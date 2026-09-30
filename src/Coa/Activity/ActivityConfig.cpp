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
}  // namespace

bool ActivityConfig::KindEnabled(ActivityKindId kind) const
{
    switch (kind)
    {
        case ActivityKindId::Duel:
            return duelEnable;
        case ActivityKindId::Gang:
            return gangEnable;
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
