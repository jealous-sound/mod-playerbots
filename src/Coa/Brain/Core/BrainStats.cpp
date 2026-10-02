/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainStats.h"
#include "BrainCache.h"
#include "BrainConfig.h"
#include "BrainGovernor.h"
#include "BrainJson.h"
#include "BrainService.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "InProcessBrain.h"
#include <string_view>

namespace
{
// JSON keys are the kebab-case names with '-' turned into '_'.
void AppendKey(std::string& out, std::string_view name)
{
    std::string key(name);
    for (char& c : key)
        if (c == '-')
            c = '_';
    BrainJson::AppendString(out, key);
    out += ':';
}

void AppendBool(std::string& out, bool value) { out += value ? "true" : "false"; }

void AppendNumber(std::string& out, uint64 value) { out += std::to_string(value); }

template <typename Counters, typename NameOf>
void AppendCounters(std::string& out, Counters const& counters, NameOf name)
{
    out += '{';
    for (std::size_t i = 0; i < counters.size(); ++i)
    {
        if (i)
            out += ',';
        AppendKey(out, name(i));
        AppendNumber(out, counters[i].load(std::memory_order_relaxed));
    }
    out += '}';
}
}  // namespace

char const* BrainFallbackName(BrainFallback fallback)
{
    switch (fallback)
    {
        case BrainFallback::Submit:
            return "submit";
        case BrainFallback::Timeout:
            return "timeout";
        case BrainFallback::Error:
            return "error";
        case BrainFallback::Invalid:
            return "invalid";
        case BrainFallback::AiOptOut:
            return "ai-optout";
        case BrainFallback::WhisperCap:
            return "whisper-cap";
        case BrainFallback::Busy:
            return "busy";
        default:
            return "unknown";
    }
}

char const* BrainTextFallbackName(BrainTextFallback fallback)
{
    switch (fallback)
    {
        case BrainTextFallback::Submit:
            return "submit";
        case BrainTextFallback::Timeout:
            return "timeout";
        case BrainTextFallback::Error:
            return "error";
        case BrainTextFallback::Refused:
            return "refused";
        case BrainTextFallback::Invalid:
            return "invalid";
        case BrainTextFallback::Repeat:
            return "repeat";
        case BrainTextFallback::Filtered:
            return "filtered";
        case BrainTextFallback::Busy:
            return "busy";
        case BrainTextFallback::SenderCap:
            return "sender-cap";
        case BrainTextFallback::OptOut:
            return "opt-out";
        default:
            return "unknown";
    }
}

BrainTextFallback BrainTextFallbackOf(BrainError error)
{
    switch (error)
    {
        case BrainError::Refused:
            return BrainTextFallback::Refused;
        case BrainError::Rejected:
            return BrainTextFallback::Invalid;
        case BrainError::Timeout:
        case BrainError::TooLate:
            return BrainTextFallback::Timeout;
        default:
            return BrainTextFallback::Error;
    }
}

BrainStats& BrainStats::Instance()
{
    static BrainStats instance;
    return instance;
}

namespace BrainStatus
{
void AppendJson(std::string& out)
{
    COA_ASSERT_WORLD_THREAD();
    std::shared_ptr<InProcessBrain> const brain = BrainService::Running();
    if (!brain)
    {
        out += "{\"enabled\":false}";
        return;
    }

    BrainStats const& stats = BrainStats::Instance();
    std::shared_ptr<BrainConfig const> const config = brain->Config();
    uint64 const now = CoaTime::NowMs();
    BrainGovernor const& governor = BrainGovernor::Instance();
    BrainBudgetStatus const budget = governor.Budget(now);
    std::vector<BrainEngineStatus> const engines = governor.EngineStatus(now);

    out += "{\"enabled\":true,\"accepting\":";
    AppendBool(out, brain->Accepting());
    out += ",\"chain\":[";
    for (std::size_t i = 0; config && i < config->engines.size(); ++i)
    {
        if (i)
            out += ',';
        BrainJson::AppendString(out, config->engines[i].name);
    }
    out += "],\"workers\":";
    AppendNumber(out, brain->WorkerCount());
    out += ",\"in_flight\":";
    AppendNumber(out, stats.inFlight.load(std::memory_order_relaxed));

    out += ",\"queue\":{";
    for (std::size_t lane = 0; lane < std::size_t(BrainLane::Count); ++lane)
    {
        if (lane)
            out += ',';
        AppendKey(out, BrainLaneName(BrainLane(lane)));
        AppendNumber(out, brain->QueueDepth(BrainLane(lane)));
    }
    out += '}';

    out += ",\"budget\":{\"day\":";
    AppendNumber(out, budget.day);
    out += ",\"usd_micros\":";
    AppendNumber(out, budget.spentMicros);
    out += ",\"reserved_micros\":";
    AppendNumber(out, budget.reservedMicros);
    out += ",\"cap_micros\":";
    AppendNumber(out, budget.capMicros);
    out += ",\"requests\":";
    AppendNumber(out, budget.requests);
    out += ",\"probes\":";
    AppendNumber(out, budget.probes);
    out += '}';

    out += ",\"engines\":[";
    for (std::size_t i = 0; i < engines.size(); ++i)
    {
        BrainEngineStatus const& engine = engines[i];
        if (i)
            out += ',';
        out += "{\"name\":";
        BrainJson::AppendString(out, engine.name);
        out += ",\"dialect\":";
        BrainJson::AppendString(out, engine.dialect);
        out += ",\"usable\":";
        AppendBool(out, engine.usable);
        out += ",\"breaker\":";
        BrainJson::AppendString(out, BrainBreakerName(engine.breaker));
        out += ",\"exhausted\":";
        AppendBool(out, engine.exhausted);
        out += ",\"auth_failed\":";
        AppendBool(out, engine.authFailed);
        out += ",\"misconfigured\":";
        AppendBool(out, engine.misconfigured);
        out += ",\"requests\":";
        AppendNumber(out, engine.today.requests);
        out += ",\"failures\":";
        AppendNumber(out, engine.today.failures);
        out += ",\"usd_micros\":";
        AppendNumber(out, engine.today.usdMicros);
        out += ",\"p50_ms\":";
        AppendNumber(out, engine.p50Ms);
        out += ",\"p95_ms\":";
        AppendNumber(out, engine.p95Ms);
        out += '}';
    }
    out += ']';

    out += ",\"submits\":";
    AppendCounters(out, stats.submits, [](std::size_t i) { return BrainSubmitName(BrainSubmit(i)); });
    out += ",\"results\":";
    AppendCounters(out, stats.results, [](std::size_t i) { return BrainErrorName(BrainError(i)); });

    out += ",\"voice\":{\"deferred\":";
    AppendNumber(out, stats.deferred.load(std::memory_order_relaxed));
    out += ",\"by_answer\":";
    AppendNumber(out, stats.byAnswer.load(std::memory_order_relaxed));
    out += ",\"by_timeout\":";
    AppendNumber(out, stats.byTimeout.load(std::memory_order_relaxed));
    out += ",\"replied\":";
    AppendNumber(out, stats.brainReplied.load(std::memory_order_relaxed));
    out += ",\"silent\":";
    AppendNumber(out, stats.brainSilent.load(std::memory_order_relaxed));
    out += ",\"ranked\":";
    AppendNumber(out, stats.ranked.load(std::memory_order_relaxed));
    out += ",\"stale\":";
    AppendNumber(out, stats.stale.load(std::memory_order_relaxed));
    out += ",\"fallbacks\":";
    AppendCounters(out, stats.fallbacks, [](std::size_t i) { return BrainFallbackName(BrainFallback(i)); });
    out += '}';

    out += ",\"text\":{\"provider\":";
    BrainJson::AppendString(out, config->text.providerName);
    if (config->text.provider != BrainTextProvider::None)
    {
        BrainEngineStatus text;
        for (BrainEngineStatus const& engine : engines)
            if (engine.name == BRAIN_TEXT_ENGINE)
                text = engine;
        out += ",\"model\":";
        BrainJson::AppendString(out, config->text.model);
        out += ",\"usable\":";
        AppendBool(out, brain->TextUsable());
        out += ",\"problem\":";
        BrainJson::AppendString(out, config->text.problem);
        out += ",\"workers\":";
        AppendNumber(out, brain->TextWorkerCount());
        out += ",\"queue\":{\"player\":";
        AppendNumber(out, brain->TextQueueDepth(BrainLane::Player));
        out += ",\"organizer\":";
        AppendNumber(out, brain->TextQueueDepth(BrainLane::Organizer));
        out += ",\"ambient\":";
        AppendNumber(out, brain->TextQueueDepth(BrainLane::Ambient));
        out += "},\"engine\":{\"breaker\":";
        BrainJson::AppendString(out, BrainBreakerName(text.breaker));
        out += ",\"exhausted\":";
        AppendBool(out, text.exhausted);
        out += ",\"auth_failed\":";
        AppendBool(out, text.authFailed);
        out += ",\"misconfigured\":";
        AppendBool(out, text.misconfigured);
        out += ",\"requests\":";
        AppendNumber(out, text.today.requests);
        out += ",\"failures\":";
        AppendNumber(out, text.today.failures);
        out += ",\"usd_micros\":";
        AppendNumber(out, text.today.usdMicros);
        out += ",\"cap_micros\":";
        AppendNumber(out, text.capUsdMicros);
        out += ",\"p50_ms\":";
        AppendNumber(out, text.p50Ms);
        out += ",\"p95_ms\":";
        AppendNumber(out, text.p95Ms);
        out += "},\"voice\":{\"chosen\":";
        AppendNumber(out, stats.textChosen.load(std::memory_order_relaxed));
        out += ",\"asked\":";
        AppendNumber(out, stats.textAsked.load(std::memory_order_relaxed));
        out += ",\"generated\":";
        AppendNumber(out, stats.textGenerated.load(std::memory_order_relaxed));
        out += ",\"sent\":";
        AppendNumber(out, stats.textSent.load(std::memory_order_relaxed));
        out += ",\"stale\":";
        AppendNumber(out, stats.textStale.load(std::memory_order_relaxed));
        out += ",\"fallbacks\":";
        AppendCounters(out, stats.textFallbacks,
                       [](std::size_t i) { return BrainTextFallbackName(BrainTextFallback(i)); });
        out += "},\"banter\":{\"exchanges\":";
        AppendNumber(out, stats.banterExchanges.load(std::memory_order_relaxed));
        out += ",\"openers\":";
        AppendNumber(out, stats.banterOpeners.load(std::memory_order_relaxed));
        out += ",\"text\":";
        AppendNumber(out, stats.banterText.load(std::memory_order_relaxed));
        out += ",\"answers\":";
        AppendNumber(out, stats.banterAnswers.load(std::memory_order_relaxed));
        out += ",\"skipped\":";
        AppendNumber(out, stats.banterSkipped.load(std::memory_order_relaxed));
        out += ",\"emotes\":";
        AppendNumber(out, stats.banterEmotes.load(std::memory_order_relaxed));
        out += "},\"rpg\":{\"asked\":";
        AppendNumber(out, stats.rpgAsked.load(std::memory_order_relaxed));
        out += ",\"answered\":";
        AppendNumber(out, stats.rpgAnswered.load(std::memory_order_relaxed));
        out += ",\"applied\":";
        AppendNumber(out, stats.rpgApplied.load(std::memory_order_relaxed));
        out += ",\"entries\":";
        AppendNumber(out, stats.rpgEntries.load(std::memory_order_relaxed));
        out += '}';
    }
    out += '}';

    BrainCache const& cache = BrainCache::Instance();
    out += ",\"cache\":{\"size\":";
    AppendNumber(out, cache.Size());
    out += ",\"hits\":";
    AppendNumber(out, cache.Hits());
    out += ",\"misses\":";
    AppendNumber(out, cache.Misses());
    out += "},\"sink_dropped\":";
    AppendNumber(out, stats.sinkDropped.load(std::memory_order_relaxed));
    out += '}';
}
}  // namespace BrainStatus
