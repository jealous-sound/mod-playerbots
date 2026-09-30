/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CoaVoiceConfig.h"
#include "Config.h"
#include "Log.h"
#include "NameMatch.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>
#include <utility>

namespace
{
std::string const Prefix = "AiPlayerbot.CoaVoice.";

std::mutex& SnapshotLock()
{
    static std::mutex lock;
    return lock;
}

// Guarded by SnapshotLock. Holds a disabled default until the first Publish.
std::shared_ptr<CoaVoiceConfig const>& Snapshot()
{
    static std::shared_ptr<CoaVoiceConfig const> snapshot = std::make_shared<CoaVoiceConfig const>();
    return snapshot;
}

std::string Trimmed(std::string const& text)
{
    std::size_t const first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

std::string AsciiLower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return out;
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

// Signed reads, so that a negative value in the conf is clamped instead of wrapping around.
uint32 Int(std::string const& key, int32 def, int32 low, int32 high)
{
    return uint32(std::clamp(sConfigMgr->GetOption<int32>(Prefix + key, def), low, high));
}

uint32 Percent(std::string const& key, int32 def) { return Int(key, def, 0, 100); }

// Seconds in the conf (fractions allowed), milliseconds in the snapshot.
uint32 Seconds(std::string const& key, float def, float maxSeconds = 3600.0f)
{
    float seconds = sConfigMgr->GetOption<float>(Prefix + key, def);
    if (!std::isfinite(seconds) || seconds < 0.0f)
        seconds = 0.0f;
    return uint32(std::lround(std::min(seconds, maxSeconds) * 1000.0f));
}

float Fraction(std::string const& key, float def)
{
    float const value = sConfigMgr->GetOption<float>(Prefix + key, def);
    if (!std::isfinite(value))
        return def;
    return std::clamp(value, 0.0f, 1.0f);
}

// The governor forgets a bot, a speaker or a place after 30 idle minutes, so no cooldown or window may be longer.
constexpr float MaxWindowSeconds = 1800.0f;

void Ordered(uint32& low, uint32& high)
{
    if (low > high)
        std::swap(low, high);
}
}  // namespace

bool CoaVoiceConfig::ListensTo(std::string_view channelName) const
{
    std::string const name = AsciiLower(channelName);
    for (std::string const& prefix : channels)
        if (name.starts_with(prefix))
            return true;
    return false;
}

uint32 CoaVoiceConfig::ChanceFor(CoaVoiceScopeKind kind) const
{
    switch (kind)
    {
        case CoaVoiceScopeKind::Whisper:
            return chanceWhisper;
        case CoaVoiceScopeKind::Party:
        case CoaVoiceScopeKind::Raid:
            return chanceParty;
        case CoaVoiceScopeKind::Guild:
            return chanceGuild;
        case CoaVoiceScopeKind::Say:
        case CoaVoiceScopeKind::Yell:
            return chanceSay;
        case CoaVoiceScopeKind::CustomEmote:
        case CoaVoiceScopeKind::TextEmote:
            return chanceEmote;
        case CoaVoiceScopeKind::Channel:
            return chanceChannel;
        default:
            return 0;
    }
}

float CoaVoiceConfig::ThresholdFor(CoaVoiceScopeKind kind) const
{
    switch (kind)
    {
        case CoaVoiceScopeKind::Whisper:
            return thresholdWhisper;
        case CoaVoiceScopeKind::Party:
        case CoaVoiceScopeKind::Raid:
            return thresholdParty;
        case CoaVoiceScopeKind::Guild:
            return thresholdGuild;
        case CoaVoiceScopeKind::Channel:
            return thresholdChannel;
        default:
            return thresholdSay;
    }
}

std::shared_ptr<CoaVoiceConfig const> CoaVoiceConfig::Load()
{
    auto config = std::make_shared<CoaVoiceConfig>();
    config->enable = sConfigMgr->GetOption<bool>(Prefix + "Enable", false);

    // Voice off: the other keys are not read, so a conf that does not list them logs nothing new.
    if (!config->enable)
        return config;

    config->channels.clear();
    for (std::string const& channel : List("Channels", "Zone"))
        config->channels.push_back(AsciiLower(channel));

    config->shortlist = Int("Shortlist", 3, 1, 10);
    config->maxResponders = std::min(Int("MaxResponders", 1, 1, 10), config->shortlist);
    config->maxNominees = Int("MaxNominees", 64, 1, 512);

    config->chanceWhisper = Percent("HeuristicChance.Whisper", 100);
    config->chanceParty = Percent("HeuristicChance.Party", 60);
    config->chanceGuild = Percent("HeuristicChance.Guild", 30);
    config->chanceSay = Percent("HeuristicChance.Say", 25);
    config->chanceChannel = Percent("HeuristicChance.Channel", 10);
    config->chanceEmote = Percent("HeuristicChance.Emote", 50);

    config->replyDelayMinMs = Seconds("ReplyDelayMin", 4.0f);
    config->replyDelayMaxMs = Seconds("ReplyDelayMax", 12.0f);
    Ordered(config->replyDelayMinMs, config->replyDelayMaxMs);
    config->combatDelayMinMs = Seconds("ReplyDelayCombatMin", 10.0f);
    config->combatDelayMaxMs = Seconds("ReplyDelayCombatMax", 25.0f);
    Ordered(config->combatDelayMinMs, config->combatDelayMaxMs);
    config->emoteDelayMinMs = Seconds("EmoteReplyDelayMin", 1.0f);
    config->emoteDelayMaxMs = Seconds("EmoteReplyDelayMax", 3.0f);
    Ordered(config->emoteDelayMinMs, config->emoteDelayMaxMs);

    config->templateCandidates = Int("TemplateCandidates", 8, 1, 32);
    config->maxBytes = Int("MaxBytes", 240, 32, 255);
    config->blockedWords.clear();
    for (std::string const& word : List("BlockedWords", ""))
    {
        std::string folded = NameMatch::Fold(word);
        if (!folded.empty())
            config->blockedWords.push_back(std::move(folded));
    }

    config->cooldownBotMs = Seconds("Cooldown.Bot", 20.0f, MaxWindowSeconds);
    config->cooldownWhisperMs = Seconds("Cooldown.Whisper", 3.0f, MaxWindowSeconds);
    config->cooldownScopeMs = Seconds("Cooldown.Scope", 8.0f, MaxWindowSeconds);
    config->cooldownSenderMs = Seconds("Cooldown.Sender", 10.0f, MaxWindowSeconds);
    config->scopePerMinute = Int("ScopePerMinute", 6, 0, 10000);
    config->globalPerMinute = Int("GlobalPerMinute", 60, 0, 100000);

    config->botToBot = sConfigMgr->GetOption<bool>(Prefix + "BotToBot", false);
    config->botChainMax = Int("BotChainMax", 2, 0, 10);
    config->chainDecay = Fraction("ChainDecay", 0.5f);
    config->humanWindowMs = Seconds("HumanWindowSeconds", 120.0f, MaxWindowSeconds);

    config->repetitionThreshold = Fraction("Repetition.Threshold", 0.7f);
    config->repetitionHistory = Int("Repetition.History", 20, 1, 100);

    config->indexRefreshMs = std::clamp<uint32>(Seconds("IndexRefreshSeconds", 5.0f), 1000, 60000);
    config->maxEmitsPerTick = Int("MaxEmitsPerTick", 20, 1, 200);
    config->replyEmotes = sConfigMgr->GetOption<bool>(Prefix + "ReplyEmotes", true);
    config->denyFallback = sConfigMgr->GetOption<bool>(Prefix + "DenyFallback", true);

    config->decisionTimeoutMs = Int("DecisionTimeoutMs", 1500, 200, 10000);
    config->thresholdWhisper = Fraction("Threshold.Whisper", 0.35f);
    config->thresholdParty = Fraction("Threshold.Party", 0.4f);
    config->thresholdGuild = Fraction("Threshold.Guild", 0.5f);
    config->thresholdSay = Fraction("Threshold.Say", 0.5f);
    config->thresholdChannel = Fraction("Threshold.Channel", 0.65f);
    config->whisperBrainPerSenderPerDay = Int("WhisperBrainPerSenderPerDay", 50, 0, 100000);
    // The reply delay hides the brain's latency; a longer wait shows as a slower answer.
    if (config->decisionTimeoutMs > config->replyDelayMinMs)
        LOG_WARN("playerbots",
                 "CoA voice: DecisionTimeoutMs ({} ms) is longer than ReplyDelayMin ({} ms); with the brain on, "
                 "some answers will come later than the configured delay",
                 config->decisionTimeoutMs, config->replyDelayMinMs);

    std::string channels;
    for (std::string const& channel : config->channels)
        channels += (channels.empty() ? "" : ",") + channel;
    LOG_INFO("playerbots",
             "CoA voice on: channels [{}], reply chances whisper {} party {} guild {} say {} channel {} "
             "emote {}, bot-to-bot {}",
             channels, config->chanceWhisper, config->chanceParty, config->chanceGuild, config->chanceSay,
             config->chanceChannel, config->chanceEmote, config->botToBot ? "on" : "off");
    return config;
}

void CoaVoiceConfig::Publish(std::shared_ptr<CoaVoiceConfig const> config)
{
    if (!config)
        config = std::make_shared<CoaVoiceConfig const>();

    bool const enable = config->enable;
    bool const botToBot = config->enable && config->botToBot;
    {
        std::lock_guard<std::mutex> guard(SnapshotLock());
        Snapshot() = std::move(config);
    }
    enabledFlag.store(enable, std::memory_order_relaxed);
    botToBotFlag.store(botToBot, std::memory_order_relaxed);
}

std::shared_ptr<CoaVoiceConfig const> CoaVoiceConfig::Current()
{
    std::lock_guard<std::mutex> guard(SnapshotLock());
    return Snapshot();
}
