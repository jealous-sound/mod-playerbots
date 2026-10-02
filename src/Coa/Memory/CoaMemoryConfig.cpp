/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaMemoryConfig.h"
#include "Config.h"
#include "Log.h"
#include "NameMatch.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <utility>

namespace
{
std::string const Prefix = "AiPlayerbot.CoaMemory.";

std::mutex& SnapshotLock()
{
    static std::mutex lock;
    return lock;
}

// Guarded by SnapshotLock. Holds a disabled default until the first Publish.
std::shared_ptr<CoaMemoryConfig const>& Snapshot()
{
    static std::shared_ptr<CoaMemoryConfig const> snapshot = std::make_shared<CoaMemoryConfig const>();
    return snapshot;
}

// The lock-free copies Publish keeps for hooks and map threads.
std::atomic<bool> enabledFlag{false};
std::atomic<bool> trackFlag{false};
std::atomic<uint8> bgKillsFlag{1};
std::atomic<uint32> behaviourMask{0};
std::array<std::atomic<uint32>, MEMORY_LEADER_ENTRIES_MAX> leaderFlags{};
std::atomic<uint32> leaderCount{0};  // stored last (release): the entries below it are complete

std::string Trimmed(std::string const& text)
{
    std::size_t const first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

std::vector<std::string> List(std::string const& key, std::string const& def)
{
    std::vector<std::string> out;
    std::istringstream items(sConfigMgr->GetOption<std::string>(Prefix + key, def));
    for (std::string item; std::getline(items, item, ',');)
    {
        std::string trimmed = Trimmed(item);
        if (!trimmed.empty())
            out.push_back(std::move(trimmed));
    }
    return out;
}

// An entry holding ASCII punctuation or symbols (":)", "<3", "^^") is matched as a raw substring of the folded
// text; any other entry as whole folded words, kept here with one space between them.
bool IsSymbolEntry(std::string_view entry)
{
    for (char const c : entry)
    {
        unsigned char const u = static_cast<unsigned char>(c);
        if (u < 0x80 && u > ' ' && !std::isalnum(u))
            return true;
    }
    return false;
}

std::vector<std::string> WordList(std::string const& key, std::string const& def)
{
    std::vector<std::string> out;
    for (std::string const& entry : List(key, def))
    {
        std::string folded;
        if (IsSymbolEntry(entry))
            folded = NameMatch::Fold(entry);
        else
        {
            for (std::string const& word : NameMatch::Prepared(entry).Words())
                folded += (folded.empty() ? "" : " ") + word;
        }
        if (!folded.empty() && std::find(out.begin(), out.end(), folded) == out.end())
            out.push_back(std::move(folded));
    }
    return out;
}

// Signed reads, so that a negative value in the conf is clamped instead of wrapping around.
uint32 Int(std::string const& key, int32 def, int32 low, int32 high)
{
    return uint32(std::clamp(sConfigMgr->GetOption<int32>(Prefix + key, def), low, high));
}

float Days(std::string const& key, float def)
{
    float const value = sConfigMgr->GetOption<float>(Prefix + key, def);
    if (!std::isfinite(value))
        return def;
    return std::clamp(value, 0.0f, 3650.0f);
}

std::string const DefaultFriendlyWords =
    "merci,mercii,thanks,thank you,thx,ty,tyvm,cimer,gg,gj,wp,bravo,bien joue,good job,well played,sympa,cool,"
    "genial,super,parfait,top,nice,trop fort,trop forte,t es fort,t es forte,love,adore,bisous,<3,:),:D,^^";
std::string const DefaultHostileWords =
    "noob,nub,n00b,naze,con,conne,connard,connasse,idiot,idiote,debile,abruti,abrutie,cretin,boulet,tocard,bouffon,"
    "clown,stfu,stupid,moron,loser,fdp,tg,ta gueule,ferme la,you suck,t es nul,t es nulle,tu es nul,pathetique,"
    "pathetic,degage,casse toi,go away,hate you,:(,-_-";
}  // namespace

char const* MemoryBehaviourName(MemoryBehaviour behaviour)
{
    switch (behaviour)
    {
        case MemoryBehaviour::GreetByMemory:
            return "GreetByMemory";
        case MemoryBehaviour::MemoryReplies:
            return "MemoryReplies";
        case MemoryBehaviour::Revenge:
            return "Revenge";
        case MemoryBehaviour::RivalDuels:
            return "RivalDuels";
        case MemoryBehaviour::FriendInvites:
            return "FriendInvites";
        default:
            return "None";
    }
}

std::shared_ptr<CoaMemoryConfig const> CoaMemoryConfig::Load()
{
    auto config = std::make_shared<CoaMemoryConfig>();
    config->enable = sConfigMgr->GetOption<bool>(Prefix + "Enable", false);

    // Memory off: the other keys are not read, so a conf that does not list them logs nothing new.
    if (!config->enable)
    {
        config->decay = MemoryDecay::FromHalfLives(config->halfLifeDays);
        return config;
    }

    // Flush before purge, flush before load and mine's pre-flush rely on the queue's order: one async worker. Read
    // once, as the pool's thread count is fixed at startup.
    static uint8 const dbWorkers = sConfigMgr->GetOption<uint8>("PlayerbotsDatabase.WorkerThreads", 1);
    if (dbWorkers != 1)
    {
        LOG_ERROR("playerbots.memory", "CoA memory stays off: it needs PlayerbotsDatabase.WorkerThreads = 1 (is {})",
                  uint32(dbWorkers));
        config->enable = false;
        config->decay = MemoryDecay::FromHalfLives(config->halfLifeDays);
        return config;
    }

    config->randomBotsOnly = sConfigMgr->GetOption<bool>(Prefix + "RandomBotsOnly", true);
    config->trackBotToBot = sConfigMgr->GetOption<bool>(Prefix + "TrackBotToBot", false);
    config->maxRelationsPerBot = Int("MaxRelationsPerBot", 64, 8, 1024);
    config->maxEventsPerBot = Int("MaxEventsPerBot", 200, 16, 5000);
    config->maxEventsLoaded = std::min(Int("MaxEventsLoaded", 64, 8, 512), config->maxEventsPerBot);
    config->flushIntervalMs = Int("FlushIntervalSeconds", 120, 10, 3600) * 1000;
    config->flushBotsPerTick = Int("FlushBotsPerTick", 20, 1, 1000);
    config->drainPerTick = Int("DrainPerTick", 2000, 100, 100000);
    config->eventRetentionDays = Int("EventRetentionDays", 30, 1, 3650);
    config->relationRetentionDays = Int("RelationRetentionDays", 180, 1, 3650);
    config->retentionBatch = Int("RetentionBatch", 5000, 100, 100000);

    static char const* const HalfLifeKeys[MEMORY_AXIS_COUNT] = {
        "HalfLifeDays.Affinity",    "HalfLifeDays.Trust",      "HalfLifeDays.Respect", "HalfLifeDays.Rivalry",
        "HalfLifeDays.Gratitude",   "HalfLifeDays.Grudge",     "HalfLifeDays.Fear",    "HalfLifeDays.Annoyance",
        "HalfLifeDays.Familiarity", "HalfLifeDays.Camaraderie"};
    for (std::size_t i = 0; i < MEMORY_AXIS_COUNT; ++i)
        config->halfLifeDays[i] = Days(HalfLifeKeys[i], config->halfLifeDays[i]);
    config->decay = MemoryDecay::FromHalfLives(config->halfLifeDays);

    config->dailyChatDeltaCap = Int("DailyChatDeltaCap", 15, 0, 100);
    config->sentiment = uint8(Int("Sentiment", 1, 0, 2));
    config->useBrainRating = sConfigMgr->GetOption<bool>("AiPlayerbot.CoaBrain.Use.Rating", false);
    config->friendlyWords = WordList("FriendlyWords", DefaultFriendlyWords);
    config->hostileWords = WordList("HostileWords", DefaultHostileWords);
    config->stateTokenBudget = Int("StateTokenBudget", 300, 0, 2000);
    config->battlegroundKills = uint8(Int("BattlegroundKills", 1, 0, 2));

    for (std::string const& item : List("LeaderEntries", ""))
    {
        char* end = nullptr;
        unsigned long const entry = std::strtoul(item.c_str(), &end, 10);
        if (!end || *end || !entry || entry > 0xFFFFFFFFul)
        {
            LOG_WARN("playerbots.memory", "CoA memory: LeaderEntries item '{}' is not a creature entry, skipped", item);
            continue;
        }
        if (config->leaderEntries.size() >= MEMORY_LEADER_ENTRIES_MAX)
        {
            LOG_WARN("playerbots.memory", "CoA memory: LeaderEntries keeps the first {} entries",
                     MEMORY_LEADER_ENTRIES_MAX);
            break;
        }
        if (std::find(config->leaderEntries.begin(), config->leaderEntries.end(), uint32(entry)) ==
            config->leaderEntries.end())
            config->leaderEntries.push_back(uint32(entry));
    }

    config->orphanSweepOnStartup = sConfigMgr->GetOption<bool>(Prefix + "OrphanSweepOnStartup", true);
    for (std::size_t i = 0; i < MEMORY_BEHAVIOUR_COUNT; ++i)
        config->behaviours[i] = sConfigMgr->GetOption<bool>(Prefix + MemoryBehaviourName(MemoryBehaviour(i)), false);

    if (config->useBrainRating && config->sentiment != 2)
        LOG_WARN("playerbots.memory",
                 "CoA memory: AiPlayerbot.CoaBrain.Use.Rating = 1 needs AiPlayerbot.CoaMemory.Sentiment = 2; the "
                 "brain does not rate messages");

    std::string behaviours;
    for (std::size_t i = 0; i < MEMORY_BEHAVIOUR_COUNT; ++i)
        if (config->behaviours[i])
            behaviours += std::string(behaviours.empty() ? "" : ",") + MemoryBehaviourName(MemoryBehaviour(i));
    LOG_DEBUG("playerbots.memory",
              "CoA memory config: {} relations and {} events per bot, sentiment {}, bot-to-bot {}, behaviours [{}]",
              config->maxRelationsPerBot, config->maxEventsPerBot, uint32(config->sentiment),
              config->trackBotToBot ? "on" : "off", behaviours);
    return config;
}

void CoaMemoryConfig::Publish(std::shared_ptr<CoaMemoryConfig const> config)
{
    if (!config)
        config = std::make_shared<CoaMemoryConfig const>();

    bool const enable = config->enable;
    bool const track = enable && config->trackBotToBot;
    uint8 const bgKills = config->battlegroundKills;
    uint32 mask = 0;
    for (std::size_t i = 0; i < MEMORY_BEHAVIOUR_COUNT; ++i)
        if (config->BehaviourOn(MemoryBehaviour(i)))
            mask |= 1u << i;

    // Readers see a count of 0 while the entries change, then the complete list.
    std::size_t const leaders = enable ? std::min(config->leaderEntries.size(), MEMORY_LEADER_ENTRIES_MAX) : 0;
    leaderCount.store(0, std::memory_order_release);
    for (std::size_t i = 0; i < leaders; ++i)
        leaderFlags[i].store(config->leaderEntries[i], std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> guard(SnapshotLock());
        Snapshot() = std::move(config);
    }

    trackFlag.store(track, std::memory_order_relaxed);
    bgKillsFlag.store(bgKills, std::memory_order_relaxed);
    behaviourMask.store(mask, std::memory_order_relaxed);
    leaderCount.store(uint32(leaders), std::memory_order_release);
    enabledFlag.store(enable, std::memory_order_relaxed);
}

std::shared_ptr<CoaMemoryConfig const> CoaMemoryConfig::Current()
{
    std::lock_guard<std::mutex> guard(SnapshotLock());
    return Snapshot();
}

bool CoaMemoryConfig::Enabled() { return enabledFlag.load(std::memory_order_relaxed); }

bool CoaMemoryConfig::TrackBotToBot() { return trackFlag.load(std::memory_order_relaxed); }

uint8 CoaMemoryConfig::BattlegroundKills() { return bgKillsFlag.load(std::memory_order_relaxed); }

bool CoaMemoryConfig::IsLeaderEntry(uint32 entry)
{
    uint32 const count = leaderCount.load(std::memory_order_acquire);
    for (uint32 i = 0; i < count; ++i)
        if (leaderFlags[i].load(std::memory_order_relaxed) == entry)
            return true;
    return false;
}

bool CoaMemoryConfig::BehaviourActive(MemoryBehaviour behaviour)
{
    return (behaviourMask.load(std::memory_order_relaxed) >> uint32(behaviour)) & 1u;
}

bool CoaMemoryConfig::AnyBehaviour() { return behaviourMask.load(std::memory_order_relaxed) != 0; }
