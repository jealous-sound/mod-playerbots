/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_COAVOICECONFIG_H
#define PLAYERBOTS_COAVOICECONFIG_H

#include "CoaVoiceEvent.h"
#include "Define.h"
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// AiPlayerbot.CoaVoice.* as one immutable snapshot. Load builds it on the world thread in
// CoaWorldScript::OnAfterConfigLoad (startup and .reload config); Publish swaps it in under a small mutex;
// readers copy the shared_ptr and keep it for the tick. Times are held in milliseconds.
struct CoaVoiceConfig
{
    bool enable = false;
    std::vector<std::string> channels{"zone"};  // lower-case channel name prefixes, like CoaLfgChannels

    uint32 maxResponders = 1;  // answers per event
    uint32 shortlist = 3;      // best-scored candidates tried in turn
    uint32 maxNominees = 64;   // candidates kept per event (named bots always kept)

    // Percent chance that an event of that scope gets an answer when no bot is named in it.
    uint32 chanceWhisper = 100;
    uint32 chanceParty = 60;  // party and raid
    uint32 chanceGuild = 30;
    uint32 chanceSay = 25;  // say and yell
    uint32 chanceChannel = 10;
    uint32 chanceEmote = 50;  // text emotes aimed at a bot, "/e" naming one

    uint32 replyDelayMinMs = 4000;
    uint32 replyDelayMaxMs = 12000;
    uint32 combatDelayMinMs = 10000;  // the answering bot is in combat when the answer is decided
    uint32 combatDelayMaxMs = 25000;
    uint32 emoteDelayMinMs = 1000;  // answers to text emotes
    uint32 emoteDelayMaxMs = 3000;

    uint32 templateCandidates = 8;          // text rows tried per answer
    uint32 maxBytes = 240;                  // byte cap of a line, cut on a UTF-8 boundary
    std::vector<std::string> blockedWords;  // folded with NameMatch::Fold; a line holding one is not said

    uint32 cooldownBotMs = 20000;     // a bot's public answers
    uint32 cooldownWhisperMs = 3000;  // a bot's whisper answers, and a whisper scope
    uint32 cooldownScopeMs = 8000;    // any other scope
    uint32 cooldownSenderMs = 10000;  // public answers to the same speaker
    uint32 scopePerMinute = 6;
    uint32 globalPerMinute = 60;

    bool botToBot = false;
    uint32 botChainMax = 2;         // deepest bot-to-bot hop answered
    float chainDecay = 0.5f;        // the chance is multiplied by this per hop
    uint32 humanWindowMs = 120000;  // bot-to-bot needs a real player heard in the scope this recently

    float repetitionThreshold = 0.7f;  // word Jaccard at which a line counts as a repeat
    uint32 repetitionHistory = 20;     // lines remembered per scope

    uint32 indexRefreshMs = 5000;  // candidate index rebuild period
    uint32 maxEmitsPerTick = 20;
    uint32 maxPending = 2048;  // lines waiting; more are dropped (PendingFull)
    bool replyEmotes = true;   // bots may play an emote with their answer
    bool denyFallback = true;  // a stranger's whisper the voice leaves unanswered gets a localized refusal

    // Used only with AiPlayerbot.CoaBrain.Enable = 1.
    uint32 decisionTimeoutMs = 1500;  // a message waits this long for the brain, then the heuristic answers
    // A calibrated engine's reply p (times ChainDecay per hop) must reach the scope's threshold for a bot to answer.
    float thresholdWhisper = 0.35f;
    float thresholdParty = 0.4f;  // party and raid
    float thresholdGuild = 0.5f;
    float thresholdSay = 0.5f;  // say, yell and emotes
    float thresholdChannel = 0.65f;
    uint32 whisperBrainPerSenderPerDay = 50;  // brain decisions per speaker account and UTC day for whispers; 0: no cap

    // M6, the text lane: a generated line instead of a template one (needs CoaBrain.Enable = 1 and a usable
    // CoaBrain.Text.Provider; its policy lives here, its transport in CoaBrain.Text.*).
    uint32 textLlm = 0;               // TextLlm: 0 off; 1 whispers and party, raid and guild chat to a real player;
                                      // 2 also say, yell and channel chat
    uint32 textTimeoutMs = 6000;      // TextTimeoutMs [1000, 30000]: a line waits this long for its text, then the
                                      // template goes
    uint32 textPerSenderPerDay = 40;  // TextPerSenderPerDay [0, 100000]: generated lines per speaker account and
                                      // UTC day; 0: no cap

    // M6, banter: ambient lines in the zone channel where a real player is. Off by default.
    bool banterEnable = false;                // Banter.Enable
    uint32 banterMaxPerMinutePerZone = 4;     // Banter.MaxPerMinutePerZone [1, 20]: every bot line in that zone's
                                              // channel counts (openers and answers)
    uint32 banterIntervalMs = 90000;          // Banter.IntervalSeconds [15, 3600]: mean time between two exchanges of
                                              // one eligible zone
    uint32 banterMinGapMs = 30000;            // Banter.MinGapSeconds [5, 600]: least time between two exchanges there
    uint32 banterMaxZones = 3;                // Banter.MaxZones [1, 20]: exchanges under way at once, realm-wide
    uint32 banterAnswerPercent = 50;          // Banter.AnswerPercent [0, 100]: a second bot answers (needs BotToBot)
    uint32 banterTextPercent = 50;            // Banter.TextPercent [0, 100]: an opener uses the text lane (needs
                                              // TextLlm = 2)
    uint32 banterQuietAfterHumanMs = 20000;   // Banter.QuietAfterHumanSeconds [0, 300]: no banter in a channel this
                                              // long after a real player spoke in it (the players get the replies)

    // Whether a channel of that name is answered (case-insensitive prefix match on `channels`).
    bool ListensTo(std::string_view channelName) const;

    // Percent chance for an unnamed event of that scope (chanceSay for Yell, chanceParty for Raid...).
    uint32 ChanceFor(CoaVoiceScopeKind kind) const;

    // The brain's reply threshold for that scope (thresholdSay for Yell and the emotes, thresholdParty for Raid).
    float ThresholdFor(CoaVoiceScopeKind kind) const;

    // M6. Whether TextLlm lets that scope have generated lines: 1 -> Whisper, Party, Raid, Guild; 2 -> also Say, Yell,
    // Channel. Never the emotes (CustomEmote, TextEmote).
    bool TextLaneFor(CoaVoiceScopeKind kind) const
    {
        switch (kind)
        {
            case CoaVoiceScopeKind::Whisper:
            case CoaVoiceScopeKind::Party:
            case CoaVoiceScopeKind::Raid:
            case CoaVoiceScopeKind::Guild:
                return textLlm >= 1;
            case CoaVoiceScopeKind::Say:
            case CoaVoiceScopeKind::Yell:
            case CoaVoiceScopeKind::Channel:
                return textLlm >= 2;
            default:
                return false;
        }
    }

    // World thread. Reads every key through sConfigMgr->GetOption and clamps the values (see the contract).
    static std::shared_ptr<CoaVoiceConfig const> Load();

    // World thread. Makes `config` current and updates the Enabled / BotToBotEnabled flags.
    static void Publish(std::shared_ptr<CoaVoiceConfig const> config);

    // Any thread. The current snapshot; a default (disabled) one before the first Publish.
    static std::shared_ptr<CoaVoiceConfig const> Current();

    // Any thread, relaxed atomics: the hooks and the seams in legacy code test these before anything else.
    static bool Enabled() { return enabledFlag.load(std::memory_order_relaxed); }
    static bool BotToBotEnabled() { return botToBotFlag.load(std::memory_order_relaxed); }

private:
    inline static std::atomic<bool> enabledFlag{false};
    inline static std::atomic<bool> botToBotFlag{false};  // enable && botToBot
};

#endif
