/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainConfig.h"
#include "BrainHttpClient.h"
#include "MockTextEngine.h"
#include "TextModelCaps.h"
#include "CoaThread.h"
#include "Config.h"
#include "Log.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "SystemOneDialects.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <utility>

namespace
{
std::string const Prefix = "AiPlayerbot.CoaBrain.";

std::mutex& SnapshotLock()
{
    static std::mutex lock;
    return lock;
}

// Guarded by SnapshotLock. Holds a disabled default until the first Publish.
std::shared_ptr<BrainConfig const>& Snapshot()
{
    static std::shared_ptr<BrainConfig const> snapshot = std::make_shared<BrainConfig const>();
    return snapshot;
}

std::string Trimmed(std::string_view text)
{
    std::size_t const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return "";
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

std::string AsciiLower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return out;
}

bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
char Upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }

// Signed reads, so that a negative value in the conf is clamped instead of wrapping around.
uint32 Int(std::string const& key, int32 def, int32 low, int32 high, bool showLogs = true)
{
    return uint32(std::clamp(sConfigMgr->GetOption<int32>(key, def, showLogs), low, high));
}

float Fraction(std::string const& key, float def, bool showLogs = true)
{
    float const value = sConfigMgr->GetOption<float>(key, def, showLogs);
    if (!std::isfinite(value))
        return def;
    return std::clamp(value, 0.0f, 1.0f);
}

// Money and prices are read as decimal text: through a float, 0.042 would become 0.041999999.
double Decimal(std::string const& key, double def, double low, double high, bool showLogs = true)
{
    std::string const text = Trimmed(sConfigMgr->GetOption<std::string>(key, Acore::StringFormat("{}", def), showLogs));
    Optional<double> const value = Acore::StringTo<double>(text);
    if (!value || !std::isfinite(*value))
    {
        if (showLogs)
            LOG_ERROR("playerbots.brain", "CoA brain: {} = '{}' is not a number; {} is used", key, text, def);
        return def;
    }
    return std::clamp(*value, low, high);
}

uint64 Micros(double usd) { return uint64(std::llround(std::max(usd, 0.0) * 1e6)); }

std::vector<std::string> SplitList(std::string const& text)
{
    std::vector<std::string> out;
    std::istringstream items(text);
    for (std::string item; std::getline(items, item, ',');)
        out.push_back(Trimmed(item));
    return out;
}

// [a-z0-9_-]{1,32}; a leading '_' is kept for the usage table's own rows ("_global", "_probe").
bool ValidEngineName(std::string const& name)
{
    if (name.empty() || name.size() > 32 || name[0] == '_')
        return false;
    for (char const c : name)
        if (!((c >= 'a' && c <= 'z') || IsDigit(c) || c == '_' || c == '-'))
            return false;
    return true;
}

BrainEngineConfig LoadEngine(std::string const& name, bool privacySendPlayerText)
{
    BrainEngineConfig engine;
    engine.name = name;
    std::string const prefix = Prefix + "Engine." + name + ".";
    // Per-engine keys are optional for every name the chain may list: no "Missing property" line.
    auto text = [&prefix](char const* key, std::string const& def)
    { return Trimmed(sConfigMgr->GetOption<std::string>(prefix + key, def, false)); };
    auto problem = [&engine](std::string why)
    {
        if (engine.problem.empty())
            engine.problem = std::move(why);
    };

    std::string const defaultDialect = SystemOneDialects::Find(name) ? name : std::string();
    engine.dialectName = AsciiLower(text("Dialect", defaultDialect));
    std::shared_ptr<SystemOneDialect const> const dialect =
        engine.dialectName.empty() ? nullptr : SystemOneDialects::Find(engine.dialectName);
    if (!dialect)
    {
        engine.problem = engine.dialectName.empty() ? "no dialect (set " + prefix + "Dialect)"
                                                    : "unknown dialect '" + engine.dialectName + "'";
        return engine;
    }
    engine.dialect = dialect->kind;
    if (!dialect->built)
        problem(dialect->name + " is not built yet (M2b)");

    engine.url = text("Url", dialect->defaultUrl);
    if (dialect->network)
    {
        BrainUrl url;
        std::string error;
        if (!ParseBrainUrl(engine.url, url, error))
            problem("bad URL: " + error);
        else
        {
            engine.tls = url.tls;
            engine.host = url.host;
            engine.port = url.port;
            engine.target = url.target;
            engine.hostIsIp = url.hostIsIp;
            engine.loopback = url.loopback;
            if (dialect->hosted && !url.tls)
                problem("hosted engines need https");
            if (dialect->loopbackOnly && !url.loopback)
                problem(dialect->name + " must use a loopback address");
        }
    }
    else
        engine.tls = false;

    engine.model = text("Model", dialect->defaultModel);

    // The key: a named environment variable first, then the conf value (the core already applies the AC_ override).
    std::string const keyName = prefix + "ApiKey";
    std::string const keyEnvName = BrainConfig::EnvVarName(keyName);
    std::string const customEnv = text("ApiKeyEnv", "");
    if (!customEnv.empty())
    {
        if (char const* value = std::getenv(customEnv.c_str()); value && *value)
        {
            engine.apiKey = Trimmed(value);
            engine.apiKeySource = "env " + customEnv;
        }
    }
    if (engine.apiKey.empty())
    {
        engine.apiKey = Trimmed(sConfigMgr->GetOption<std::string>(keyName, "", false));
        if (engine.apiKey.empty())
            engine.apiKeySource.clear();
        else if (std::getenv(keyEnvName.c_str()))
            engine.apiKeySource = "env " + keyEnvName;
        else
        {
            engine.apiKeySource = "conf";
            LOG_WARN("playerbots.brain",
                     "CoA brain: the API key of engine {} is written in the conf file; prefer the environment "
                     "variable {}",
                     name, keyEnvName);
        }
    }
    if (dialect->requiresKey && engine.apiKey.empty())
        problem("no API key (set " + keyName + " or the environment variable " + keyEnvName + ")");

    bool const sendFlag = sConfigMgr->GetOption<bool>(prefix + "SendPlayerText", engine.loopback, false);
    engine.sendPlayerText = sendFlag && (engine.loopback || privacySendPlayerText);
    if (engine.sendPlayerText && !engine.loopback)
        LOG_WARN("playerbots.brain", "CoA brain: engine {}: player words are sent to {}", name, engine.host);

    engine.calibrated = sConfigMgr->GetOption<bool>(prefix + "Calibrated", dialect->calibratedByDefault, false);
    engine.uncalibratedConfidence = Fraction(prefix + "UncalibratedConfidence", 0.3f, false);
    engine.priceInPerMTok = Decimal(prefix + "PriceInPerMTok", dialect->hosted ? 0.042 : 0.0, 0.0, 1000.0, false);
    engine.priceOutPerMTok = Decimal(prefix + "PriceOutPerMTok", 0.0, 0.0, 1000.0, false);
    engine.maxUsdMicrosPerDay = Micros(Decimal(prefix + "MaxUsdPerDay", 1.0, 0.0, 1000.0, false));
    engine.maxRequestsPerDay = Int(prefix + "MaxRequestsPerDay", 20000, 0, 10000000, false);

    if (dialect->kind == BrainDialect::Mock)
    {
        engine.mockLatencyMs = Int(prefix + "LatencyMs", 150, 0, 30000, false);
        engine.mockErrorPercent = Int(prefix + "ErrorPercent", 0, 0, 100, false);
        int32 const status = sConfigMgr->GetOption<int32>(prefix + "FailStatus", 0, false);
        engine.mockFailStatus = status >= 100 && status <= 599 ? uint32(status) : 0;
        engine.mockUsdMicrosPerCall = Micros(Decimal(prefix + "UsdPerCall", 0.0, 0.0, 10.0, false));
    }

    engine.usable = engine.problem.empty();
    return engine;
}
BrainTextConfig LoadText(bool privacySendPlayerText)
{
    BrainTextConfig config;
    std::string const prefix = Prefix + "Text.";
    auto text = [&prefix](char const* key, std::string const& def)
    { return Trimmed(sConfigMgr->GetOption<std::string>(prefix + key, def, false)); };
    std::string const provider = AsciiLower(Trimmed(sConfigMgr->GetOption<std::string>(prefix + "Provider", "none")));
    if (provider.empty() || provider == "none")
        return config;
    if (provider == "openai")
        config.provider = BrainTextProvider::OpenAi;
    else if (provider == "anthropic")
        config.provider = BrainTextProvider::Anthropic;
    else if (provider == "mock")
        config.provider = BrainTextProvider::Mock;
    else
    {
        config.problem = "unknown provider '" + provider + "' (none, openai, anthropic or mock)";
        LOG_ERROR("playerbots.brain", "CoA brain text generator: {}", config.problem);
        return config;
    }
    config.providerName = BrainTextProviderName(config.provider);
    auto problem = [&config](std::string why)
    {
        if (config.problem.empty())
            config.problem = std::move(why);
    };
    bool const mock = config.provider == BrainTextProvider::Mock;
    bool const anthropic = config.provider == BrainTextProvider::Anthropic;
    config.url = text("Url", mock ? "" : anthropic ? "https://api.anthropic.com/v1/messages"
                                                 : "http://127.0.0.1:11434/v1/chat/completions");
    if (config.url.empty() && !mock)
        config.url = anthropic ? "https://api.anthropic.com/v1/messages"
                               : "http://127.0.0.1:11434/v1/chat/completions";
    if (!mock)
    {
        BrainUrl url;
        std::string error;
        if (!ParseBrainUrl(config.url, url, error))
            problem("bad URL: " + error);
        else
        {
            config.tls = url.tls;
            config.host = url.host;
            config.port = url.port;
            config.target = url.target;
            config.hostIsIp = url.hostIsIp;
            config.loopback = url.loopback;
            if (!url.loopback && !url.tls)
                problem("hosted providers need https");
        }
    }
    else
        config.tls = false;
    config.model = text("Model", mock ? "mock" : anthropic ? "claude-opus-5" : "");
    if (config.model.empty() && (anthropic || mock))
        config.model = anthropic ? "claude-opus-5" : "mock";
    if (config.model.empty())
        problem("no model (set AiPlayerbot.CoaBrain.Text.Model)");

    std::string const keyName = prefix + "ApiKey";
    std::string const keyEnvName = BrainConfig::EnvVarName(keyName);
    std::string const customEnv = text("ApiKeyEnv", "");
    if (!customEnv.empty())
        if (char const* value = std::getenv(customEnv.c_str()); value && *value)
        {
            config.apiKey = Trimmed(value);
            config.apiKeySource = "env " + customEnv;
        }
    if (config.apiKey.empty())
    {
        config.apiKey = text("ApiKey", "");
        if (config.apiKey.empty())
            config.apiKeySource.clear();
        else if (std::getenv(keyEnvName.c_str()))
            config.apiKeySource = "env " + keyEnvName;
        else
        {
            config.apiKeySource = "conf";
            LOG_WARN("playerbots.brain", "CoA brain: the text API key is written in the conf file; prefer {}",
                     keyEnvName);
        }
    }
    if (!mock && config.apiKey.empty() && (anthropic || !config.loopback))
        problem("no API key (set " + keyName + " or the environment variable " + keyEnvName + ")");
    bool const sendFlag = sConfigMgr->GetOption<bool>(prefix + "SendPlayerText", config.loopback, false);
    config.sendPlayerText = sendFlag && (config.loopback || privacySendPlayerText);
    if (config.sendPlayerText && !config.loopback && !mock)
        LOG_WARN("playerbots.brain", "CoA brain: the text generator sends players' words to {}", config.host);

    config.maxTokens = Int(prefix + "MaxTokens", 1024, 16, 8192, false);
    config.timeoutMs = Int(prefix + "TimeoutMs", 6000, 500, 60000, false);
    config.workers = Int(prefix + "Workers", 1, 1, 4, false);
    config.maxUsdMicrosPerDay = Micros(Decimal(prefix + "MaxUsdPerDay", 0.5, 0.0, 1000.0, false));
    config.maxRequestsPerDay = Int(prefix + "MaxRequestsPerDay", 500, 0, 10000000, false);
    TextModelCaps const caps = anthropic ? TextModelCaps::For(config.model) : TextModelCaps{};
    config.priceInPerMTok = Decimal(prefix + "PriceInPerMTok", caps.priceInPerMTok, 0.0, 1000.0, false);
    config.priceOutPerMTok = Decimal(prefix + "PriceOutPerMTok", caps.priceOutPerMTok, 0.0, 1000.0, false);
    if (anthropic && !caps.known)
        LOG_WARN("playerbots.brain", "CoA brain: no price known for model {}: spend is counted from the reported cost "
                 "and the byte estimate only", config.model);
    std::string const temperature = text("Temperature", "");
    if (!temperature.empty())
    {
        Optional<double> const value = Acore::StringTo<double>(temperature);
        if (value && std::isfinite(*value))
        {
            config.hasTemperature = true;
            config.temperature = float(std::clamp(*value, 0.0, 2.0));
        }
    }
    config.reasoningEffort = AsciiLower(text("ReasoningEffort", ""));
    if (!config.reasoningEffort.empty() &&
        ((config.reasoningEffort.size() > 32 ||
          config.reasoningEffort.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) ||
         (anthropic && config.reasoningEffort != "low" && config.reasoningEffort != "medium" &&
          config.reasoningEffort != "high")))
    {
        LOG_ERROR("playerbots.brain", "CoA brain: invalid Text.ReasoningEffort '{}' ignored", config.reasoningEffort);
        config.reasoningEffort.clear();
    }
    config.anthropicServerFallbacks = sConfigMgr->GetOption<bool>(prefix + "Anthropic.ServerFallbacks", true, false);
    config.anthropicPromptCache = sConfigMgr->GetOption<bool>(prefix + "Anthropic.PromptCache", true, false);
    if (mock)
    {
        config.mockLatencyMs = Int(prefix + "Mock.LatencyMs", 150, 0, 30000, false);
        config.mockFault = AsciiLower(text("Mock.Fault", "none"));
        MockTextFault const fault = MockTextFaultFromName(config.mockFault);
        if (config.mockFault != MockTextFaultName(fault))
        {
            LOG_ERROR("playerbots.brain", "CoA brain: unknown Text.Mock.Fault '{}' ignored", config.mockFault);
            config.mockFault = "none";
        }
        config.mockFaultPercent = Int(prefix + "Mock.FaultPercent", 0, 0, 100, false);
        config.mockUsdMicrosPerCall = Micros(Decimal(prefix + "Mock.UsdPerCall", 0.0, 0.0, 10.0, false));
    }
    config.usable = config.problem.empty();
    if (config.usable)
        LOG_INFO("playerbots.brain", "CoA brain text generator: provider {}, model {}, host {}, budget ${:.2f}/day, "
                 "workers {}", config.providerName, config.model, config.host, double(config.maxUsdMicrosPerDay) / 1e6,
                 config.workers);
    else
        LOG_INFO("playerbots.brain", "CoA brain text generator: provider {} unusable: {}", config.providerName,
                 config.problem);
    return config;
}
}  // namespace

char const* BrainTextProviderName(BrainTextProvider provider)
{
    switch (provider)
    {
        case BrainTextProvider::None:
            return "none";
        case BrainTextProvider::OpenAi:
            return "openai";
        case BrainTextProvider::Anthropic:
            return "anthropic";
        case BrainTextProvider::Mock:
            return "mock";
        default:
            return "none";
    }
}

char const* BrainDialectName(BrainDialect dialect)
{
    switch (dialect)
    {
        case BrainDialect::OpenRouter:
            return "openrouter";
        case BrainDialect::TypeSafe:
            return "typesafe";
        case BrainDialect::JevLocal:
            return "jevlocal";
        case BrainDialect::Mock:
            return "mock";
        case BrainDialect::OpenAiConstrained:
            return "openai-constrained";
        default:
            return "unknown";
    }
}

BrainEngineConfig const* BrainConfig::Find(std::string_view name) const
{
    for (BrainEngineConfig const& engine : engines)
        if (engine.name == name)
            return &engine;
    return nullptr;
}

bool BrainConfig::AnyUsable() const
{
    for (BrainEngineConfig const& engine : engines)
        if (engine.usable)
            return true;
    return false;
}

std::string BrainConfig::ChainText() const
{
    std::string out;
    for (BrainEngineConfig const& engine : engines)
    {
        if (!out.empty())
            out += ',';
        out += engine.name;
    }
    return out;
}

std::shared_ptr<BrainConfig const> BrainConfig::Load(bool forceEnable)
{
    COA_ASSERT_WORLD_THREAD();
    auto config = std::make_shared<BrainConfig>();
    config->enable = sConfigMgr->GetOption<bool>(Prefix + "Enable", false);

    // Brain off: no other key is read, so a conf that does not list them logs nothing new.
    if (!config->enable && !forceEnable)
        return config;
    config->enable = true;

    config->dialectFile = Trimmed(sConfigMgr->GetOption<std::string>(Prefix + "DialectFile", ""));
    SystemOneDialects::Reload(ResolvePath(config->dialectFile));

    config->maxUsdMicrosPerDay = Micros(Decimal(Prefix + "MaxUsdPerDay", 1.5, 0.0, 1000.0));
    config->workers = Int(Prefix + "Workers", 2, 2, 16);
    config->connectTimeoutMs = Int(Prefix + "ConnectTimeoutMs", 2000, 100, 30000);
    config->readTimeoutMs = Int(Prefix + "ReadTimeoutMs", 4000, 200, 60000);
    config->dnsRefreshSeconds = Int(Prefix + "DnsRefreshSeconds", 300, 30, 86400);
    config->workerDeadlineMs = Int(Prefix + "Shutdown.WorkerDeadlineMs", 2000, 0, 30000);
    config->maxRequestsPerMinute = Int(Prefix + "MaxRequestsPerMinute", 120, 1, 100000);

    std::vector<std::string> const stops = SplitList(sConfigMgr->GetOption<std::string>(Prefix + "LaneStopAt",
                                                                                         "100,80,50"));
    for (std::size_t i = 0; i < stops.size() && i < config->laneStopPercent.size(); ++i)
    {
        Optional<int32> const percent = Acore::StringTo<int32>(stops[i]);
        if (percent && *percent >= 0 && *percent <= 100)
            config->laneStopPercent[i] = uint32(*percent);
    }

    config->breakerFailures = Int(Prefix + "Breaker.Failures", 5, 1, 100);
    config->breakerOpenSeconds = Int(Prefix + "Breaker.OpenSeconds", 30, 1, 3600);
    config->cacheSize = Int(Prefix + "CacheSize", 20000, 0, 1000000);
    config->cacheTtlSeconds = Int(Prefix + "CacheTtlSeconds", 600, 0, 86400);
    config->stateMaxChars = Int(Prefix + "StateMaxChars", 3000, 500, 24000);
    config->privacySendPlayerText = sConfigMgr->GetOption<bool>(Prefix + "Privacy.SendPlayerText", false);
    config->useChatReply = sConfigMgr->GetOption<bool>(Prefix + "Use.ChatReply", true);
    config->questionFile = Trimmed(sConfigMgr->GetOption<std::string>(Prefix + "QuestionFile", ""));
    config->fixtureDir = Trimmed(sConfigMgr->GetOption<std::string>(Prefix + "FixtureDir", ""));
    config->probeMaxPerDay = Int(Prefix + "Probe.MaxPerDay", 300, 0, 100000);
    config->probeRecordDir = Trimmed(sConfigMgr->GetOption<std::string>(Prefix + "Probe.RecordDir", ""));
    config->caBundle = Trimmed(sConfigMgr->GetOption<std::string>(Prefix + "CaBundle", ""));
    config->tlsVerify = sConfigMgr->GetOption<bool>(Prefix + "TlsVerify", true);

    std::vector<std::string> names;
    for (std::string const& item : SplitList(sConfigMgr->GetOption<std::string>(Prefix + "Decision.Chain",
                                                                                "openrouter")))
    {
        if (item.empty())
            continue;
        std::string const name = AsciiLower(item);
        if (name == BRAIN_TEXT_ENGINE)
        {
            LOG_ERROR("playerbots.brain", "CoA brain: engine name 'text' is reserved for the text generator");
            continue;
        }
        if (!ValidEngineName(name))
        {
            LOG_ERROR("playerbots.brain",
                      "CoA brain: engine name '{}' in {}Decision.Chain is ignored (use a-z, 0-9, '_' and '-', at "
                      "most 32 characters, not starting with '_')",
                      item, Prefix);
            continue;
        }
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    }
    for (std::string const& name : names)
        config->engines.push_back(LoadEngine(name, config->privacySendPlayerText));

    config->useRpgNext = sConfigMgr->GetOption<bool>(Prefix + "Use.RpgNext", false);
    config->useEmote = sConfigMgr->GetOption<bool>(Prefix + "Use.Emote", false);
    config->rpgInfluence = Fraction(Prefix + "Rpg.Influence", 0.5f);
    config->rpgAdviceTtlSeconds = Int(Prefix + "Rpg.AdviceTtlSeconds", 600, 60, 86400);
    config->rpgMinBots = Int(Prefix + "Rpg.MinBots", 3, 1, 100);
    config->text = LoadText(config->privacySendPlayerText);

    std::string chain;
    for (BrainEngineConfig const& engine : config->engines)
    {
        if (!chain.empty())
            chain += ", ";
        chain += engine.name + "(" + (engine.usable ? std::string("ok") : engine.problem) + ")";
    }
    LOG_INFO("playerbots.brain", "CoA brain config: chain {}, budget ${:.2f}/day, workers {}",
             chain.empty() ? std::string("(empty)") : chain, double(config->maxUsdMicrosPerDay) / 1e6,
             config->workers);
    return config;
}

void BrainConfig::Publish(std::shared_ptr<BrainConfig const> config)
{
    COA_ASSERT_WORLD_THREAD();
    if (!config)
        return;
    std::lock_guard<std::mutex> guard(SnapshotLock());
    Snapshot() = std::move(config);
}

std::shared_ptr<BrainConfig const> BrainConfig::Current()
{
    std::lock_guard<std::mutex> guard(SnapshotLock());
    return Snapshot();
}

std::string BrainConfig::Mask(std::string_view secret)
{
    if (secret.empty())
        return "(none)";
    if (secret.size() < 12)
        return "****";
    return std::string(secret.substr(0, 4)) + "..." + std::string(secret.substr(secret.size() - 4));
}

// The core's IniKeyToEnvVarKey (Config.cpp, anonymous namespace), replicated: "aB" -> "A_B", "a1" -> "A_1",
// "1a" -> "1_A", separators -> '_'.
std::string BrainConfig::EnvVarName(std::string_view key)
{
    std::string out = "AC_";
    for (std::size_t i = 0; i < key.size(); ++i)
    {
        char const c = key[i];
        if (c == ' ' || c == '.' || c == '-')
        {
            out += '_';
            continue;
        }
        if (i + 1 < key.size())
        {
            char const next = key[i + 1];
            if ((!IsUpper(c) && IsUpper(next)) || (!IsDigit(c) && IsDigit(next)) || (IsDigit(c) && !IsDigit(next)))
            {
                out += Upper(c);
                out += '_';
                continue;
            }
        }
        out += Upper(c);
    }
    return out;
}

std::string BrainConfig::ResolvePath(std::string const& path)
{
    if (path.empty())
        return path;
    try
    {
        if (std::filesystem::path(path).is_absolute())
            return path;
    }
    catch (...)
    {
        return path;
    }
    return sConfigMgr->GetConfigPath() + path;
}
