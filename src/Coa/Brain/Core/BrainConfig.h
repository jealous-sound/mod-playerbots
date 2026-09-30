/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINCONFIG_H
#define PLAYERBOTS_BRAINCONFIG_H

#include "Define.h"
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Kinds of decision engine. The per-kind defaults (URL, model, key, loopback, calibration) and every wire field
// name live in the dialect table, SystemOneDialects.cpp.
enum class BrainDialect : uint8
{
    OpenRouter = 0,     // https://openrouter.ai/api/alpha/decisions, OpenRouter key
    TypeSafe,           // https://api.typesafe.ai/v1/systemone, TypeSafe key (waitlisted)
    JevLocal,           // the same wire on a loopback address, no key (M2b adapter)
    Mock,               // replays data/coa/brain/fixtures, or answers at random; no network
    OpenAiConstrained,  // llama.cpp / vLLM constrained decoding: M2b, answers NotBuilt for now
    Unknown
};

char const* BrainDialectName(BrainDialect dialect);  // "openrouter", "typesafe", "jevlocal", "mock", ...

// One engine of Decision.Chain, from AiPlayerbot.CoaBrain.Engine.<name>.*.
struct BrainEngineConfig
{
    std::string name;  // [a-z0-9_-]{1,32}: also the `engine` column of playerbots_coa_brain_usage
    BrainDialect dialect = BrainDialect::Unknown;
    std::string dialectName;

    std::string url;  // as configured (or the dialect's default)
    bool tls = true;  // parsed from url
    std::string host;
    uint16 port = 443;
    std::string target = "/";
    bool hostIsIp = false;
    bool loopback = false;  // 127.0.0.0/8, ::1 or localhost

    std::string model;
    std::string apiKey;        // never logged or printed: see Mask
    std::string apiKeySource;  // "conf", "env AC_...", "env <ApiKeyEnv>", or "" when there is none

    // Effective: Engine.<n>.SendPlayerText and (a loopback engine, or Privacy.SendPlayerText = 1).
    bool sendPlayerText = false;
    bool calibrated = true;               // Engine.<n>.Calibrated
    float uncalibratedConfidence = 0.3f;  // the p given to an answer that carries no provider probability

    double priceInPerMTok = 0.042;  // USD per million input tokens, when the response reports no cost
    double priceOutPerMTok = 0.0;
    uint64 maxUsdMicrosPerDay = 1000000;
    uint32 maxRequestsPerDay = 20000;

    // Mock only.
    uint32 mockLatencyMs = 150;
    uint32 mockErrorPercent = 0;       // calls answered with mockFailStatus (or a 500) instead
    uint32 mockFailStatus = 0;         // HTTP status of those failures; 0 means 500
    uint64 mockUsdMicrosPerCall = 0;   // reported as usage.cost

    bool usable = false;  // false: skipped by the chain; `problem` says why (status shows it)
    std::string problem;
};

// The text generator (M6): where short chat lines come from. Not a decision engine: it is not in Decision.Chain, has
// its own keys (AiPlayerbot.CoaBrain.Text.*), workers and queue, and shows in the governor as the engine "text".
enum class BrainTextProvider : uint8
{
    None = 0,   // Text.Provider = none (the default): nothing in this family runs
    OpenAi,     // an OpenAI-compatible POST {Url} (Ollama, llama.cpp, vLLM, LM Studio, OpenRouter, DeepSeek, Groq...)
    Anthropic,  // POST https://api.anthropic.com/v1/messages (raw HTTP, x-api-key)
    Mock        // no network: scripted replies and injected faults, for tests and soaks
};

char const* BrainTextProviderName(BrainTextProvider provider);  // "none", "openai", "anthropic", "mock"

// AiPlayerbot.CoaBrain.Text.* as read by BrainConfig::Load, with its clamps applied (see the contract).
struct BrainTextConfig
{
    BrainTextProvider provider = BrainTextProvider::None;
    std::string providerName = "none";

    std::string url;  // as configured, or the provider's default
    bool tls = true;  // parsed from url
    std::string host;
    uint16 port = 443;
    std::string target = "/";
    bool hostIsIp = false;
    bool loopback = false;  // 127.0.0.0/8, ::1 or localhost

    std::string model;         // Anthropic: the model id, claude-opus-5 by default; OpenAI: required
    std::string apiKey;        // never logged or printed: see BrainConfig::Mask
    std::string apiKeySource;  // "conf", "env AC_...", "env <ApiKeyEnv>", or "" when there is none

    // Effective: Text.SendPlayerText and (a loopback URL, or CoaBrain.Privacy.SendPlayerText = 1). Without it the
    // voice lane never applies (the model could not answer without the words); banter sends no player word.
    bool sendPlayerText = false;

    uint32 maxTokens = 1024;   // Text.MaxTokens [16, 8192]
    uint32 timeoutMs = 6000;   // Text.TimeoutMs [500, 60000]: one call's whole budget, capped by the job's time left
    uint32 workers = 1;        // Text.Workers [1, 4]: threads serving the text queue

    double priceInPerMTok = 0.0;   // USD per million input tokens, when the response reports no cost
    double priceOutPerMTok = 0.0;
    uint64 maxUsdMicrosPerDay = 500000;  // Text.MaxUsdPerDay (0.50)
    uint32 maxRequestsPerDay = 500;      // Text.MaxRequestsPerDay

    bool hasTemperature = false;  // Text.Temperature set (OpenAI always; Anthropic only for models that allow it)
    float temperature = 0.0f;
    std::string reasoningEffort;  // Text.ReasoningEffort: OpenAI "reasoning_effort"; Anthropic low|medium|high

    bool anthropicServerFallbacks = true;  // Text.Anthropic.ServerFallbacks (claude-opus-5 only)
    bool anthropicPromptCache = true;      // Text.Anthropic.PromptCache (known models only)

    // Mock only (Text.Mock.*).
    uint32 mockLatencyMs = 150;
    std::string mockFault = "none";  // none, refusal, timeout, 402, 429, 500, garbage, empty, injection, long, mixed
    uint32 mockFaultPercent = 0;
    uint64 mockUsdMicrosPerCall = 0;

    bool usable = false;  // false: no text job is accepted; `problem` says why (status shows it)
    std::string problem;
};

// AiPlayerbot.CoaBrain.* as one immutable snapshot. Load builds it on the world thread (CoaWorldScript's
// OnAfterConfigLoad, and .playerbots brain on); BrainService::Apply publishes the snapshot it runs; readers
// copy the shared_ptr. Times are held in milliseconds, money in micro-USD.
struct BrainConfig
{
    bool enable = false;

    std::vector<BrainEngineConfig> engines;  // Decision.Chain order, duplicates removed, unusable ones kept

    uint64 maxUsdMicrosPerDay = 1500000;  // MaxUsdPerDay: every engine, every lane, probes included
    uint32 workers = 2;                   // at least 2
    uint32 connectTimeoutMs = 2000;
    uint32 readTimeoutMs = 4000;
    uint32 dnsRefreshSeconds = 300;
    uint32 workerDeadlineMs = 2000;  // Shutdown.WorkerDeadlineMs: then the workers are detached
    uint32 maxRequestsPerMinute = 120;
    std::array<uint32, 3> laneStopPercent{100, 80, 50};  // LaneStopAt: Player, Organizer, Ambient
    uint32 breakerFailures = 5;
    uint32 breakerOpenSeconds = 30;
    uint32 cacheSize = 20000;  // 0 turns the cache off
    uint32 cacheTtlSeconds = 600;
    uint32 stateMaxChars = 3000;  // bytes of state text, player text included; 500 to 24000
    bool privacySendPlayerText = false;
    bool useChatReply = true;
    uint32 probeMaxPerDay = 300;

    // M6, read only with Enable = 1.
    BrainTextConfig text;              // Text.*
    bool useRpgNext = false;           // Use.RpgNext: BrainRpgAdvice asks which NewRpg status comes next
    bool useEmote = false;             // Use.Emote: a banter opener's emote is asked of the brain
    float rpgInfluence = 0.5f;         // Rpg.Influence [0, 1]: the weight of the advice in NewRpg's status pick
    uint32 rpgAdviceTtlSeconds = 600;  // Rpg.AdviceTtlSeconds [60, 86400]
    uint32 rpgMinBots = 3;             // Rpg.MinBots [1, 100]: random bots a group needs before it is asked about

    std::string questionFile;    // "" = compiled-in wording; relative paths are under the config directory
    std::string dialectFile;     // "" = compiled-in dialect table
    std::string fixtureDir;      // mock engine and .playerbots brain test selftest; "" = compiled-in
    std::string probeRecordDir;  // "" = .playerbots brain probe <engine> record is refused
    std::string caBundle;        // PEM file of trusted roots; "" = the system's
    bool tlsVerify = true;

    BrainEngineConfig const* Find(std::string_view name) const;
    bool AnyUsable() const;                   // an engine of Decision.Chain is usable
    bool TextUsable() const { return text.usable; }
    bool AnyUsableOrText() const { return AnyUsable() || text.usable; }  // the brain starts when this holds
    std::string ChainText() const;  // "openrouter,mock"

    // World thread. Reads Enable; when it is 0 (and forceEnable is false) returns at once, so a conf without the
    // other keys logs nothing. Otherwise reads DialectFile first and calls SystemOneDialects::Reload with it,
    // then every key, clamped (see the contract), and warns once per load for each API key written in the conf.
    static std::shared_ptr<BrainConfig const> Load(bool forceEnable = false);

    // World thread (BrainService::Apply). Makes `config` current.
    static void Publish(std::shared_ptr<BrainConfig const> config);

    // Any thread. The current snapshot; a default (disabled) one before the first Publish.
    static std::shared_ptr<BrainConfig const> Current();

    // "sk-o...9f3a" (first 4, last 4) for 12 bytes or more, "****" below, "(none)" when empty.
    static std::string Mask(std::string_view secret);

    // The core's environment override name for a config key: "AC_" + its IniKeyToEnvVarKey
    // ("AiPlayerbot.CoaBrain.Engine.openrouter.ApiKey" -> "AC_AI_PLAYERBOT_COA_BRAIN_ENGINE_OPENROUTER_API_KEY").
    static std::string EnvVarName(std::string_view key);

    // A path as given when absolute or empty, else under sConfigMgr->GetConfigPath().
    static std::string ResolvePath(std::string const& path);
};

#endif
