/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainAdmin.h"
#include "BrainCache.h"
#include "BrainConfig.h"
#include "BrainDecisionRing.h"
#include "BrainGovernor.h"
#include "BrainJson.h"
#include "BrainSelfTest.h"
#include "BrainService.h"
#include "BrainSink.h"
#include "BrainStats.h"
#include "BrainUsageStore.h"
#include "CoaThread.h"
#include "CoaTime.h"
#include "Config.h"
#include "IBrain.h"
#include "InProcessBrain.h"
#include "QuestionRegistry.h"
#include "StringFormat.h"
#include "SystemOneDialects.h"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

namespace
{
constexpr uint32 SINK_CAPACITY = 256;
constexpr uint64 TEST_DEADLINE_MS = 10000;
constexpr uint64 PROBE_DEADLINE_MS = 15000;
constexpr uint64 REPORT_AFTER_MS = 60000;
constexpr uint32 EXPLAIN_ENTRIES = 10;
constexpr std::size_t RECORD_PARSE_BYTES = 256 * 1024;

struct PendingTest
{
    uint64 tag = 0;
    uint64 jobId = 0;
    uint32 requester = 0;
    uint64 startedMs = 0;
    std::string botName;
    std::string scope;
    float threshold = 0.35f;
    std::vector<std::pair<std::string, std::string>> lines;
};

struct PendingCase
{
    uint64 tag = 0;
    uint64 jobId = 0;
    std::string caseId;
    std::string locale;
    bool expectReply = true;
    std::string expectGroup;
    std::vector<std::pair<std::string, std::string>> lineGroups;
    bool received = false;
    BrainResult result;
};

struct PendingProbe
{
    bool active = false;
    uint32 requester = 0;
    std::string engine;
    std::string dialect;
    std::string url;
    bool record = false;
    std::string recordDir;
    uint64 startedMs = 0;
    std::vector<PendingCase> cases;
    uint32 received = 0;
};

// World thread only, like every BrainAdmin entry.
struct AdminState
{
    std::shared_ptr<BrainSink> sink;
    std::vector<PendingTest> tests;
    PendingProbe probe;
    uint64 nextTag = 1;
    std::vector<BrainResult> drained;  // reused by Update
};

AdminState& State()
{
    static AdminState state;
    return state;
}

std::shared_ptr<BrainSink> const& Sink()
{
    AdminState& state = State();
    if (!state.sink)
        state.sink = std::make_shared<BrainSink>(SINK_CAPACITY);
    return state.sink;
}

std::string Usd(uint64 micros, int decimals = 4)
{
    return decimals == 6 ? Acore::StringFormat("${:.6f}", double(micros) / 1e6)
                         : Acore::StringFormat("${:.4f}", double(micros) / 1e6);
}

std::string DayText(uint32 day)
{
    if (!day)
        return "-";
    return Acore::StringFormat("{:04}-{:02}-{:02}", day / 10000, (day / 100) % 100, day % 100);
}

std::string P(float p) { return Acore::StringFormat("{:.2f}", p); }

std::string Lower(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return text;
}

std::string Trimmed(std::string const& text)
{
    std::size_t const first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// "queued=12 off=3", zero counters skipped; "none" when every counter is zero.
template <typename Counters, typename NameOf>
std::string CounterText(Counters const& counters, NameOf name)
{
    std::string out;
    for (std::size_t i = 0; i < counters.size(); ++i)
    {
        uint64 const value = counters[i].load(std::memory_order_relaxed);
        if (!value)
            continue;
        if (!out.empty())
            out += ' ';
        out += Acore::StringFormat("{}={}", name(i), value);
    }
    return out.empty() ? "none" : out;
}

std::string VoiceText(BrainStats const& stats)
{
    std::pair<char const*, uint64> const values[] = {
        {"deferred", stats.deferred.load(std::memory_order_relaxed)},
        {"answered", stats.byAnswer.load(std::memory_order_relaxed)},
        {"timed-out", stats.byTimeout.load(std::memory_order_relaxed)},
        {"replied", stats.brainReplied.load(std::memory_order_relaxed)},
        {"silent", stats.brainSilent.load(std::memory_order_relaxed)},
        {"ranked", stats.ranked.load(std::memory_order_relaxed)},
        {"stale", stats.stale.load(std::memory_order_relaxed)},
    };
    std::string out;
    for (auto const& [name, value] : values)
    {
        if (!value)
            continue;
        if (!out.empty())
            out += ' ';
        out += Acore::StringFormat("{}={}", name, value);
    }
    return out.empty() ? "none" : out;
}

std::string EngineLine(BrainEngineConfig const& engine, BrainEngineStatus const* status)
{
    if (!engine.usable)
        return Acore::StringFormat("engine {}: unusable: {}", engine.name, engine.problem);

    std::string details = engine.dialectName + ", " + engine.model;
    if (!engine.url.empty())
        details += ", " + engine.url;
    if (engine.apiKey.empty())
        details += ", no key";
    else
        details += ", key " + BrainConfig::Mask(engine.apiKey) + " from " + engine.apiKeySource;
    details += engine.calibrated ? ", calibrated" : ", uncalibrated";
    details += engine.sendPlayerText ? ", player text yes" : ", player text no";

    if (!status)
        return Acore::StringFormat("engine {} [{}]: no call yet", engine.name, details);

    std::string breaker = BrainBreakerName(status->breaker);
    if (status->breaker == BrainBreakerState::Open)
        breaker += Acore::StringFormat(" ({} s left)", (status->openForMs + 999) / 1000);

    std::string line = Acore::StringFormat(
        "engine {} [{}]: breaker {}; today {} requests, {} failures, {} of ${:.2f}, {} of {}; p50/p95 {}/{} ms ({})",
        engine.name, details, breaker, status->today.requests, status->today.failures, Usd(status->today.usdMicros),
        double(status->capUsdMicros) / 1e6, status->today.requests, status->capRequests,
        status->p50Ms, status->p95Ms, status->samples);
    if (status->exhausted)
        line += ", exhausted until UTC midnight";
    if (status->authFailed)
        line += status->markStatus ? Acore::StringFormat(", auth failed (HTTP {})", status->markStatus)
                                   : std::string(", auth failed");
    if (status->misconfigured)
        line += status->markStatus ? Acore::StringFormat(", misconfigured (HTTP {})", status->markStatus)
                                   : std::string(", misconfigured");
    for (std::string const& situation : status->disabledSituations)
        line += ", disabled for " + situation;
    return line;
}

std::string FailureText(BrainResult const& result)
{
    std::string text = BrainErrorName(result.error);
    if (result.httpStatus)
        text += Acore::StringFormat(", HTTP {}", result.httpStatus);
    if (!result.detail.empty())
        text += ": " + result.detail;
    return text;
}

// The top `count` entries of a choice answer: its distribution, or its choice alone.
std::vector<std::pair<std::string, float>> Top(BrainAnswer const& answer, std::size_t count)
{
    std::vector<std::pair<std::string, float>> top;
    if (!answer.choice.empty())
        top.emplace_back(answer.choice, answer.p);
    for (auto const& entry : answer.dist)
    {
        if (top.size() >= count)
            break;
        if (entry.first != answer.choice)
            top.push_back(entry);
    }
    if (top.size() > count)
        top.resize(count);
    return top;
}

std::string Lookup(std::vector<std::pair<std::string, std::string>> const& pairs, std::string const& key)
{
    for (auto const& [k, v] : pairs)
        if (k == key)
            return v;
    return "";
}

void PrintTest(PendingTest const& test, BrainResult const& result, BrainAdminPrint const& print)
{
    uint32 const to = test.requester;
    if (result.error != BrainError::None)
    {
        print(to, Acore::StringFormat("brain test {}: failed ({})", test.botName, FailureText(result)));
        return;
    }

    print(to, Acore::StringFormat("brain test {} ({}, threshold {}): {} {} ms, {}, {}{}", test.botName, test.scope,
                                  P(test.threshold), result.engine.empty() ? std::string("-") : result.engine,
                                  result.callMs, result.calibrated ? "calibrated" : "uncalibrated",
                                  Usd(result.usage.usdMicros, 6), result.fromCache ? " [cache]" : ""));

    if (BrainAnswer const* reply = result.Find("reply"))
    {
        if (!reply->valid)
            print(to, "reply: invalid");
        else if (!reply->calibrated)
            print(to, Acore::StringFormat("reply: p={} (uncalibrated: the heuristic decides)", P(reply->p)));
        else
            print(to, Acore::StringFormat("reply: p={} -> {}", P(reply->p),
                                          reply->p >= test.threshold ? "answers" : "stays silent"));
    }
    if (BrainAnswer const* who = result.Find("who"))
    {
        if (!who->valid)
            print(to, "who: invalid");
        else
            print(to, Acore::StringFormat("who: {} p={}", who->choice, P(who->p)));
    }
    if (BrainAnswer const* line = result.Find("line"))
    {
        if (!line->valid)
            print(to, "line: invalid");
        else
        {
            std::string text;
            for (auto const& [key, p] : Top(*line, 3))
            {
                bool const first = text.empty();
                if (!first)
                    text += " | ";
                text += Acore::StringFormat("{} p={}", key, P(p));
                // The best line is shown as the bot would say it.
                if (std::string const row = Lookup(test.lines, key); first && !row.empty())
                    text += " \"" + row + "\"";
            }
            print(to, "line: " + text);
        }
    }
    if (BrainAnswer const* tone = result.Find("tone"))
    {
        if (!tone->valid)
            print(to, "tone: invalid");
        else
        {
            std::shared_ptr<BrainQuestionSet const> const questions = QuestionRegistry::Current();
            std::string const name = tone->level >= 0 && std::size_t(tone->level) < questions->toneLevels.size()
                                         ? questions->toneLevels[std::size_t(tone->level)]
                                         : std::string("?");
            std::string text = Acore::StringFormat("tone: {} {}", tone->level, name);
            if (tone->score >= 0.0f)
                text += Acore::StringFormat(" (score {:.1f})", tone->score);
            print(to, text);
        }
    }
    if (BrainAnswer const* emote = result.Find("emote"))
    {
        if (!emote->valid)
            print(to, "emote: invalid");
        else
            print(to, Acore::StringFormat("emote: {} p={}", emote->choice, P(emote->p)));
    }
}

void AppendPretty(std::string& out, BrainJsonValue const& value, uint32 indent)
{
    if (value.IsArray() && !value.items.empty())
    {
        out += "[\n";
        for (std::size_t i = 0; i < value.items.size(); ++i)
        {
            out.append(indent + 2, ' ');
            AppendPretty(out, value.items[i], indent + 2);
            out += i + 1 < value.items.size() ? ",\n" : "\n";
        }
        out.append(indent, ' ');
        out += ']';
        return;
    }
    if (value.IsObject() && !value.members.empty())
    {
        out += "{\n";
        for (std::size_t i = 0; i < value.members.size(); ++i)
        {
            out.append(indent + 2, ' ');
            BrainJson::AppendString(out, value.members[i].key);
            out += ": ";
            AppendPretty(out, value.members[i].value, indent + 2);
            out += i + 1 < value.members.size() ? ",\n" : "\n";
        }
        out.append(indent, ' ');
        out += '}';
        return;
    }
    out += BrainJson::Serialize(value);
}

BrainJsonValue BodyValue(std::string const& body)
{
    BrainJsonValue value;
    std::string error;
    if (!body.empty() && BrainJson::Parse(body, value, error, RECORD_PARSE_BYTES))
        return value;
    BrainJsonValue raw = BrainJsonValue::MakeObject();
    raw.Set("raw", BrainJsonValue::MakeString(body));
    return raw;
}

std::string SafeFilePart(std::string text)
{
    for (char& c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            c = '_';
    return text;
}

// Writes each probe call that kept its bodies as a fixture file (format: data/coa/brain/README.md).
void RecordFixtures(PendingProbe const& probe, uint32 to, BrainAdminPrint const& print)
{
    int64 const now = int64(std::time(nullptr));
    uint32 const day = BrainClock::UtcDay(now);
    int64 const secondOfDay = now - int64(now / 86400) * 86400;
    std::string const stamp = Acore::StringFormat("{:08}T{:02}{:02}{:02}Z", day, secondOfDay / 3600,
                                                  (secondOfDay / 60) % 60, secondOfDay % 60);

    std::error_code ec;
    std::filesystem::create_directories(probe.recordDir, ec);

    uint32 written = 0;
    for (PendingCase const& probeCase : probe.cases)
    {
        BrainResult const& result = probeCase.result;
        if (!probeCase.received || (result.rawRequest.empty() && result.rawResponse.empty()))
            continue;

        BrainJsonValue root = BrainJsonValue::MakeObject();
        // Named as README.md asks the reviewed file to be renamed: <dialect>-<case>.
        root.Set("fixture", BrainJsonValue::MakeString(probe.dialect + "-" + probeCase.caseId));
        root.Set("source", BrainJsonValue::MakeString("recorded"));
        root.Set("note", BrainJsonValue::MakeString("Recorded by .playerbots brain probe on " + DayText(day) +
                                                    ". Review it, then add an expect block."));
        root.Set("dialect", BrainJsonValue::MakeString(probe.dialect));
        root.Set("url", BrainJsonValue::MakeString(result.url.empty() ? probe.url : result.url));
        root.Set("status", BrainJsonValue::MakeNumber(double(result.httpStatus)));
        root.Set("request", BodyValue(result.rawRequest));
        root.Set("response", BodyValue(result.rawResponse));
        root.Set("expect", BrainJsonValue::MakeObject());

        std::string text;
        AppendPretty(text, root, 0);
        text += '\n';

        std::filesystem::path const path = std::filesystem::path(probe.recordDir) /
                                           (SafeFilePart(probe.engine) + "-" + SafeFilePart(probeCase.caseId) + "-" +
                                            stamp + ".json");
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(text.data(), std::streamsize(text.size()));
        file.close();
        if (file)
            ++written;
        else
            print(to, "could not write " + path.string());
    }
    print(to, Acore::StringFormat("recorded {} fixtures in {}", written, probe.recordDir));
}

void PrintProbe(PendingProbe const& probe, BrainAdminPrint const& print)
{
    uint32 const to = probe.requester;
    std::vector<uint32> latencies;
    uint64 spent = 0;
    uint32 answered = 0;
    bool notFound = false;
    for (PendingCase const& probeCase : probe.cases)
    {
        if (!probeCase.received)
            continue;
        spent += probeCase.result.usage.usdMicros;
        notFound = notFound || probeCase.result.httpStatus == 404;
        if (probeCase.result.error != BrainError::None)
            continue;
        ++answered;
        latencies.push_back(probeCase.result.callMs);
    }
    std::sort(latencies.begin(), latencies.end());
    auto percentile = [&latencies](uint32 p) -> uint32
    {
        if (latencies.empty())
            return 0;
        std::size_t const n = latencies.size();
        return latencies[std::min(n - 1, n * p / 100)];
    };

    uint32 const total = uint32(probe.cases.size());
    print(to, Acore::StringFormat("probe {}: {} cases, {} answered, {} failed, p50/p95 {}/{} ms, {}", probe.engine,
                                  total, answered, total - answered, percentile(50), percentile(95), Usd(spent, 6)));

    auto lineGroup = [](PendingCase const& probeCase, std::string const& key) -> std::string
    { return key == "silent" ? std::string("silent") : Lookup(probeCase.lineGroups, key); };

    std::vector<std::string> locales;
    for (PendingCase const& probeCase : probe.cases)
        if (std::find(locales.begin(), locales.end(), probeCase.locale) == locales.end())
            locales.push_back(probeCase.locale);
    for (std::string const& locale : locales)
    {
        uint32 replies = 0;
        uint32 repliesRight = 0;
        uint32 lines = 0;
        uint32 linesRight = 0;
        double pSum = 0.0;
        for (PendingCase const& probeCase : probe.cases)
        {
            if (probeCase.locale != locale || !probeCase.received || probeCase.result.error != BrainError::None)
                continue;
            if (BrainAnswer const* reply = probeCase.result.Find("reply"); reply && reply->valid)
            {
                ++replies;
                pSum += reply->p;
                if ((reply->p >= 0.5f) == probeCase.expectReply)
                    ++repliesRight;
            }
            if (probeCase.expectGroup.empty())
                continue;
            if (BrainAnswer const* line = probeCase.result.Find("line"); line && line->valid)
            {
                ++lines;
                if (lineGroup(probeCase, line->choice) == probeCase.expectGroup)
                    ++linesRight;
            }
        }
        print(to, Acore::StringFormat("{}: reply {}/{}, line kind {}/{}, mean reply p {}",
                                      locale.empty() ? std::string("?") : locale, repliesRight, replies, linesRight,
                                      lines, replies ? P(float(pSum / replies)) : std::string("-")));
    }

    for (PendingCase const& probeCase : probe.cases)
    {
        if (!probeCase.received)
        {
            print(to, probeCase.caseId + ": no answer");
            continue;
        }
        BrainResult const& result = probeCase.result;
        if (result.error != BrainError::None)
        {
            print(to, Acore::StringFormat("{}: failed ({})", probeCase.caseId, FailureText(result)));
            continue;
        }

        char const* expected = probeCase.expectReply ? "yes" : "no";
        bool replyRight = false;
        std::string text = probeCase.caseId + ": ";
        if (BrainAnswer const* reply = result.Find("reply"); reply && reply->valid)
        {
            replyRight = (reply->p >= 0.5f) == probeCase.expectReply;
            text += Acore::StringFormat("reply p={} (expected {})", P(reply->p), expected);
        }
        else
            text += Acore::StringFormat("reply invalid (expected {})", expected);

        bool lineRight = probeCase.expectGroup.empty();
        if (BrainAnswer const* line = result.Find("line"); line && line->valid)
        {
            std::string const group = lineGroup(probeCase, line->choice);
            text += Acore::StringFormat(" line {} = {}", line->choice, group.empty() ? std::string("?") : group);
            if (!probeCase.expectGroup.empty())
            {
                text += " (expected " + probeCase.expectGroup + ")";
                lineRight = group == probeCase.expectGroup;
            }
        }
        else
            text += " line invalid";
        text += replyRight && lineRight ? " OK" : " MISS";
        print(to, text);
    }

    if (notFound)
        if (std::shared_ptr<SystemOneDialect const> const dialect = SystemOneDialects::Find(probe.dialect))
            for (std::string const& alternate : dialect->alternateUrls)
                print(to, Acore::StringFormat("HTTP 404 from {}: try AiPlayerbot.CoaBrain.Engine.{}.Url = {}",
                                              probe.url, probe.engine, alternate));

    if (probe.record)
        RecordFixtures(probe, to, print);
}
}  // namespace

namespace BrainAdmin
{
std::vector<std::string> Status()
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<std::string> lines;
    std::shared_ptr<InProcessBrain> const brain = BrainService::Running();
    std::shared_ptr<BrainConfig const> const config = brain ? brain->Config() : BrainConfig::Current();
    int32 const overrideState = BrainService::Override();
    std::string const problem = BrainService::LastProblem();

    std::string state;
    if (brain)
        state = overrideState == 1 ? "on (.playerbots brain on)" : "on (conf)";
    else if (!problem.empty())
        state = "off (" + problem + ")";
    else if (overrideState == 0)
        state = "off (.playerbots brain off)";
    else
        state = "off (conf)";

    BrainStats const& stats = BrainStats::Instance();
    std::shared_ptr<BrainQuestionSet const> const questions = QuestionRegistry::Current();
    lines.push_back(Acore::StringFormat(
        "CoA brain: {}; accepting {}; workers {}; in flight {}; queue {}/{}/{}; questions {} ({}); dialects {}", state,
        brain && brain->Accepting() ? "yes" : "no", brain ? brain->WorkerCount() : 0,
        stats.inFlight.load(std::memory_order_relaxed), brain ? brain->QueueDepth(BrainLane::Player) : 0,
        brain ? brain->QueueDepth(BrainLane::Organizer) : 0, brain ? brain->QueueDepth(BrainLane::Ambient) : 0,
        questions->version, questions->source, SystemOneDialects::Source()));

    uint64 const now = CoaTime::NowMs();
    BrainGovernor const& governor = BrainGovernor::Instance();
    BrainBudgetStatus const budget = governor.Budget(now);
    BrainUsageStore const& store = BrainUsageStore::Instance();
    char const* table = !store.Loaded() ? "not loaded" : store.TableMissing() ? "missing" : "ok";
    lines.push_back(Acore::StringFormat(
        "budget UTC {}: {} spent + {} reserved of ${:.2f} (lanes stop at {}/{}/{}%); {} requests; probes {}/{}; rate "
        "{}/{} per minute left; usage table {}",
        DayText(budget.day), Usd(budget.spentMicros), Usd(budget.reservedMicros),
        double(config->maxUsdMicrosPerDay) / 1e6, config->laneStopPercent[0], config->laneStopPercent[1],
        config->laneStopPercent[2], budget.requests, budget.probes, config->probeMaxPerDay, budget.tokens,
        config->maxRequestsPerMinute, table));

    std::vector<BrainEngineStatus> const engines = governor.EngineStatus(now);
    for (BrainEngineConfig const& engine : config->engines)
    {
        BrainEngineStatus const* status = nullptr;
        for (BrainEngineStatus const& candidate : engines)
            if (candidate.name == engine.name)
                status = &candidate;
        lines.push_back(EngineLine(engine, status));
    }

    lines.push_back("submits: " +
                    CounterText(stats.submits, [](std::size_t i) { return BrainSubmitName(BrainSubmit(i)); }));
    lines.push_back("results: " +
                    CounterText(stats.results, [](std::size_t i) { return BrainErrorName(BrainError(i)); }));
    lines.push_back("voice: " + VoiceText(stats) + "; fallbacks: " +
                    CounterText(stats.fallbacks, [](std::size_t i) { return BrainFallbackName(BrainFallback(i)); }));

    BrainCache const& cache = BrainCache::Instance();
    lines.push_back(Acore::StringFormat("cache: {} entries, hits={}, misses={}; sink drops={}", cache.Size(),
                                        cache.Hits(), cache.Misses(),
                                        stats.sinkDropped.load(std::memory_order_relaxed)));
    return lines;
}

std::vector<std::string> TurnOn()
{
    COA_ASSERT_WORLD_THREAD();
    std::shared_ptr<BrainConfig const> const config = BrainConfig::Load(true);
    QuestionRegistry::Reload(config->questionFile);
    BrainUsageStore::Instance().LoadSync();
    BrainService::SetOverride(1);
    BrainService::Apply(config);

    if (std::shared_ptr<InProcessBrain> const brain = BrainService::Running())
        return {Acore::StringFormat("CoA brain on: chain {}, workers {}, budget ${:.2f}/day", config->ChainText(),
                                    brain->WorkerCount(), double(config->maxUsdMicrosPerDay) / 1e6)};
    std::string const problem = BrainService::LastProblem();
    return {"CoA brain could not start: " + (problem.empty() ? std::string("the server is shutting down") : problem)};
}

std::vector<std::string> TurnOff()
{
    COA_ASSERT_WORLD_THREAD();
    BrainService::SetOverride(0);
    BrainService::Apply(BrainConfig::Current());
    return {"CoA brain off until .reload config or .playerbots brain on"};
}

std::vector<std::string> Reset()
{
    COA_ASSERT_WORLD_THREAD();
    BrainGovernor::Instance().ResetFlags();
    BrainCache::Instance().Clear();
    return {"breakers closed, exhausted/auth/misconfigured marks and disabled situations cleared, cache emptied"};
}

std::vector<std::string> Explain(uint64 botGuid, std::string const& botName)
{
    COA_ASSERT_WORLD_THREAD();
    std::vector<BrainDecisionEntry> const entries = BrainDecisionRing::Instance().ForBot(botGuid, EXPLAIN_ENTRIES);
    if (entries.empty())
        return {"no brain decision recorded for " + botName};

    std::vector<std::string> lines;
    lines.push_back("brain decisions of " + botName + " (newest first):");
    for (BrainDecisionEntry const& entry : entries)
    {
        uint64 const second = entry.atUnix % 86400;
        std::string line = Acore::StringFormat("{:02}:{:02}:{:02} UTC event {} {}: {}", second / 3600,
                                               (second / 60) % 60, second % 60, entry.eventId, entry.scope,
                                               entry.outcome);
        if (!entry.engine.empty())
            line += " via " + entry.engine;
        line += Acore::StringFormat(" {} ms", entry.ms);
        if (entry.calibrated)
            line += ", calibrated";
        if (!entry.reason.empty())
            line += ", reason " + entry.reason;

        std::vector<std::string> fields;
        if (entry.replyP >= 0.0f)
            fields.push_back("reply p=" + P(entry.replyP));
        if (!entry.who.empty())
            fields.push_back("who " + entry.who);
        if (!entry.line.empty())
            fields.push_back(entry.lineP >= 0.0f ? Acore::StringFormat("line {} p={}", entry.line, P(entry.lineP))
                                                 : "line " + entry.line);
        if (entry.tone >= 0)
            fields.push_back(Acore::StringFormat("tone {}", entry.tone));
        if (!entry.emote.empty())
            fields.push_back("emote " + entry.emote);
        for (std::size_t i = 0; i < fields.size(); ++i)
            line += (i ? ", " : ": ") + fields[i];
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> StartTest(uint32 requesterAccount, BrainTestJob&& test)
{
    COA_ASSERT_WORLD_THREAD();
    std::shared_ptr<IBrain> const brain = BrainService::Current();
    if (!brain->Active(BrainUse::Test))
        return {"CoA brain is off"};

    AdminState& state = State();
    uint64 const now = CoaTime::NowMs();
    BrainJob job = std::move(test.job);
    job.use = BrainUse::Test;
    job.lane = BrainLane::Player;
    job.tag = state.nextTag++;
    job.epoch = 0;
    job.deadlineMs = now + TEST_DEADLINE_MS;
    job.cacheKey.clear();
    job.forceEngine.clear();
    job.record = false;
    if (job.situation.empty())
        job.situation = "chat_reply";

    uint64 const tag = job.tag;
    BrainTicket const ticket = brain->Submit(std::move(job), Sink(), nullptr);
    if (ticket.status != BrainSubmit::Queued)
        return {Acore::StringFormat("brain test {}: refused ({})", test.botName, BrainSubmitName(ticket.status))};

    PendingTest pending;
    pending.tag = tag;
    pending.jobId = ticket.jobId;
    pending.requester = requesterAccount;
    pending.startedMs = now;
    pending.botName = test.botName;
    pending.scope = std::move(test.scope);
    pending.threshold = test.threshold;
    pending.lines = std::move(test.lines);
    state.tests.push_back(std::move(pending));
    return {Acore::StringFormat("brain test {}: asked, the answer follows", test.botName)};
}

std::vector<std::string> StartProbe(uint32 requesterAccount, std::string const& engineName, bool record,
                                    std::vector<BrainProbeJob>&& jobs)
{
    COA_ASSERT_WORLD_THREAD();
    AdminState& state = State();
    if (state.probe.active)
        return {"a probe is already running; wait for its report"};

    std::shared_ptr<IBrain> const brain = BrainService::Current();
    if (!brain->Active(BrainUse::Probe))
        return {"CoA brain is off"};

    std::shared_ptr<BrainConfig const> const config = BrainConfig::Current();
    std::string name = Lower(Trimmed(engineName));
    if (name.empty())
    {
        for (BrainEngineConfig const& engine : config->engines)
        {
            if (engine.usable)
            {
                name = engine.name;
                break;
            }
        }
        if (name.empty())
            return {"no usable engine to probe (chain: " + config->ChainText() + ")"};
    }
    BrainEngineConfig const* engine = config->Find(name);
    if (!engine)
        return {"unknown engine " + name + " (chain: " + config->ChainText() + ")"};
    if (!engine->usable)
        return {"engine " + name + " is unusable: " + engine->problem};
    if (record && config->probeRecordDir.empty())
        return {"set AiPlayerbot.CoaBrain.Probe.RecordDir to record fixtures"};
    if (jobs.empty())
        return {"no probe case to ask"};

    PendingProbe probe;
    probe.requester = requesterAccount;
    probe.engine = name;
    probe.dialect = engine->dialectName;
    probe.url = engine->url;
    probe.record = record;
    probe.recordDir = BrainConfig::ResolvePath(config->probeRecordDir);
    probe.startedMs = CoaTime::NowMs();

    BrainSubmit refusal = BrainSubmit::Queued;
    for (BrainProbeJob& probeJob : jobs)
    {
        BrainJob job = std::move(probeJob.job);
        job.use = BrainUse::Probe;
        job.lane = BrainLane::Organizer;
        job.forceEngine = name;
        job.record = record;
        job.cacheKey.clear();
        job.tag = state.nextTag++;
        job.epoch = 0;
        job.deadlineMs = probe.startedMs + PROBE_DEADLINE_MS;
        if (job.situation.empty())
            job.situation = "probe";

        uint64 const tag = job.tag;
        BrainTicket const ticket = brain->Submit(std::move(job), Sink(), nullptr);
        if (ticket.status != BrainSubmit::Queued)
        {
            refusal = ticket.status;
            break;
        }
        PendingCase pending;
        pending.tag = tag;
        pending.jobId = ticket.jobId;
        pending.caseId = std::move(probeJob.caseId);
        pending.locale = std::move(probeJob.locale);
        pending.expectReply = probeJob.expectReply;
        pending.expectGroup = std::move(probeJob.expectGroup);
        pending.lineGroups = std::move(probeJob.lineGroups);
        probe.cases.push_back(std::move(pending));
    }

    std::vector<std::string> lines;
    if (!probe.cases.empty())
    {
        lines.push_back(Acore::StringFormat("probe {}: {} cases asked", name, probe.cases.size()));
        probe.active = true;
        state.probe = std::move(probe);
    }
    if (refusal != BrainSubmit::Queued)
        lines.push_back(Acore::StringFormat("probe {}: refused after {} of {} cases ({})", name,
                                            state.probe.active ? state.probe.cases.size() : 0, jobs.size(),
                                            BrainSubmitName(refusal)));
    return lines;
}

std::vector<std::string> SelfTest()
{
    COA_ASSERT_WORLD_THREAD();
    // With the brain off the snapshot holds no FixtureDir: the key is read directly.
    std::shared_ptr<BrainConfig const> const config = BrainConfig::Current();
    std::string const dir = Trimmed(config->enable ? config->fixtureDir
                                                   : sConfigMgr->GetOption<std::string>(
                                                         "AiPlayerbot.CoaBrain.FixtureDir", "", false));
    std::string const path = BrainConfig::ResolvePath(dir);

    BrainSelfTest::Report report;
    try
    {
        report = BrainSelfTest::Run(path);
    }
    catch (std::exception const& e)
    {
        return {std::string("selftest: aborted: ") + e.what()};
    }
    catch (...)
    {
        return {"selftest: aborted"};
    }

    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat("selftest: {} passed, {} failed ({})", report.passed, report.failed,
                                        path.empty() ? std::string("compiled-in fixtures") : path));
    for (std::string& line : report.lines)
        lines.push_back(std::move(line));
    return lines;
}

void Update(uint64 nowMs, BrainAdminPrint const& print)
{
    COA_ASSERT_WORLD_THREAD();
    AdminState& state = State();
    // No sink yet (no test or probe since the start, or a turn-on that failed): nothing to drain or to time out.
    if (!state.sink && state.tests.empty() && !state.probe.active)
        return;

    state.drained.clear();
    if (state.sink)
        state.sink->Drain(state.drained);
    for (BrainResult& result : state.drained)
    {
        auto const test = std::find_if(state.tests.begin(), state.tests.end(), [&result](PendingTest const& t)
                                       { return t.tag == result.tag && t.jobId == result.jobId; });
        if (test != state.tests.end())
        {
            PrintTest(*test, result, print);
            state.tests.erase(test);
            continue;
        }
        if (!state.probe.active)
            continue;
        for (PendingCase& probeCase : state.probe.cases)
        {
            if (probeCase.tag != result.tag || probeCase.jobId != result.jobId || probeCase.received)
                continue;
            probeCase.received = true;
            probeCase.result = std::move(result);
            ++state.probe.received;
            break;
        }
    }
    state.drained.clear();

    for (auto it = state.tests.begin(); it != state.tests.end();)
    {
        if (nowMs - it->startedMs < REPORT_AFTER_MS)
        {
            ++it;
            continue;
        }
        print(it->requester, Acore::StringFormat("brain test {}: no answer within 60 s", it->botName));
        it = state.tests.erase(it);
    }

    if (state.probe.active &&
        (state.probe.received >= state.probe.cases.size() || nowMs - state.probe.startedMs >= REPORT_AFTER_MS))
    {
        PendingProbe finished = std::move(state.probe);
        state.probe = PendingProbe{};
        PrintProbe(finished, print);
    }
}

void Shutdown()
{
    COA_ASSERT_WORLD_THREAD();
    AdminState& state = State();
    if (state.sink)
        state.sink->Close();
    state.sink.reset();
    state.tests.clear();
    state.probe = PendingProbe{};
}
}  // namespace BrainAdmin
