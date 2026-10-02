/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MockDecisionEngine.h"
#include "BrainJson.h"
#include "CoaText.h"
#include "CoaTime.h"
#include "Log.h"
#include "SystemOneDialects.h"
#include "SystemOneWire.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <thread>
#include <utility>

namespace
{
constexpr std::size_t MaxFixtureBytes = 64 * 1024;
constexpr uint32 SliceMs = 20;
constexpr double ChosenP = 0.6;

std::mt19937& Rng()
{
    thread_local std::mt19937 rng(
        [] {
            std::random_device device;
            std::seed_seq seed{device(), device(), uint32(std::hash<std::thread::id>()(std::this_thread::get_id())),
                               uint32(CoaTime::NowMs())};
            return std::mt19937(seed);
        }());
    return rng;
}

uint32 Below(uint32 n) { return n ? uint32(Rng()() % n) : 0; }

double Uniform(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(Rng()); }

std::string const& WireType(SystemOneDialect const& dialect, BrainQuestionType type)
{
    switch (type)
    {
        case BrainQuestionType::YesNo:
            return dialect.typeYesNo;
        case BrainQuestionType::Score:
            return dialect.typeScore;
        case BrainQuestionType::Choice:
        default:
            return dialect.typeChoice;
    }
}

bool ReadFile(std::filesystem::path const& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    text.assign(MaxFixtureBytes + 1, '\0');
    in.read(text.data(), std::streamsize(text.size()));
    std::size_t const got = std::size_t(in.gcount());
    if (got > MaxFixtureBytes)
        return false;
    text.resize(got);
    return true;
}

// Probabilities peaked on one key: `chosen` gets 0.6, the others share the rest.
BrainJsonValue Peaked(std::vector<std::string> const& keys, std::size_t chosen)
{
    BrainJsonValue probabilities = BrainJsonValue::MakeObject();
    double const rest = keys.size() > 1 ? (1.0 - ChosenP) / double(keys.size() - 1) : 0.0;
    for (std::size_t i = 0; i < keys.size(); ++i)
        probabilities.Set(keys[i], BrainJsonValue::MakeNumber(keys.size() == 1 ? 1.0 : (i == chosen ? ChosenP : rest)));
    return probabilities;
}

std::vector<std::string> OptionKeys(BrainQuestion const& q)
{
    std::vector<std::string> keys;
    if (q.type == BrainQuestionType::Score)
        for (std::size_t i = 0; i < q.options.size(); ++i)
            keys.push_back(std::to_string(i));
    else
        for (BrainOption const& o : q.options)
            keys.push_back(o.key);
    return keys;
}

BrainJsonValue RandomAnswer(SystemOneDialect const& dialect, BrainQuestion const& q)
{
    BrainJsonValue a = BrainJsonValue::MakeObject();
    a.Set(dialect.fType, BrainJsonValue::MakeString(WireType(dialect, q.type)));
    std::vector<std::string> const keys = OptionKeys(q);

    switch (q.type)
    {
        case BrainQuestionType::Choice:
        {
            std::size_t const chosen = Below(uint32(keys.size()));
            a.Set(dialect.fChoice, BrainJsonValue::MakeString(keys.empty() ? std::string() : keys[chosen]));
            a.Set(dialect.fConfidence, BrainJsonValue::MakeNumber(keys.size() == 1 ? 1.0 : ChosenP));
            a.Set(dialect.fProbabilities, Peaked(keys, chosen));
            break;
        }
        case BrainQuestionType::YesNo:
            a.Set(dialect.fYesNo, BrainJsonValue::MakeNumber(Uniform(0.2, 0.95)));
            break;
        case BrainQuestionType::Score:
        {
            std::size_t const level = Below(uint32(keys.size()));
            a.Set(dialect.fScore, BrainJsonValue::MakeNumber(double(level)));
            a.Set(dialect.fConfidence, BrainJsonValue::MakeNumber(ChosenP));
            a.Set(dialect.fProbabilities, Peaked(keys, level));
            break;
        }
    }
    return a;
}

// The fixture's answer made to fit this job: a choice outside the job's options is redrawn among them; a noul or
// a score out of range, or a missing value, gives a random valid answer instead.
BrainJsonValue FitAnswer(SystemOneDialect const& dialect, BrainQuestion const& q, BrainJsonValue answer)
{
    std::vector<std::string> const keys = OptionKeys(q);
    switch (q.type)
    {
        case BrainQuestionType::Choice:
        {
            BrainJsonValue const* choice = answer.Find(dialect.fChoice);
            bool const known =
                choice && choice->IsString() && std::find(keys.begin(), keys.end(), choice->string) != keys.end();
            if (!known)
            {
                if (keys.empty())
                    return RandomAnswer(dialect, q);
                std::size_t const chosen = Below(uint32(keys.size()));
                answer.Set(dialect.fChoice, BrainJsonValue::MakeString(keys[chosen]));
                answer.Set(dialect.fProbabilities, Peaked(keys, chosen));
                if (answer.Find(dialect.fConfidence))
                    answer.Set(dialect.fConfidence, BrainJsonValue::MakeNumber(keys.size() == 1 ? 1.0 : ChosenP));
            }
            return answer;
        }
        case BrainQuestionType::YesNo:
        {
            double const p = answer.GetNumber(dialect.fYesNo, -1.0);
            return p >= 0.0 && p <= 1.0 ? answer : RandomAnswer(dialect, q);
        }
        case BrainQuestionType::Score:
        {
            double const s = answer.GetNumber(dialect.fScore, -1.0);
            return s >= 0.0 && s <= double(keys.size()) - 1.0 ? answer : RandomAnswer(dialect, q);
        }
    }
    return answer;
}
}  // namespace

MockDecisionEngine::MockDecisionEngine(BrainEngineConfig config, std::shared_ptr<SystemOneDialect const> dialect,
                                       std::string const& fixtureDir)
    : config(std::move(config)), dialect(std::move(dialect))
{
    if (fixtureDir.empty() || !this->dialect)
        return;

    namespace fs = std::filesystem;
    try
    {
        std::error_code ec;
        if (!fs::is_directory(fixtureDir, ec))
        {
            LOG_WARN("playerbots.brain", "CoA brain: mock engine {}: fixture directory {} not found; it answers at "
                                         "random", this->config.name, fixtureDir);
            return;
        }

        std::vector<fs::path> files;
        for (fs::directory_iterator it(fixtureDir, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code fileEc;
            if (it->is_regular_file(fileEc) && it->path().extension() == ".json")
                files.push_back(it->path());
        }
        std::sort(files.begin(), files.end(),
                  [](fs::path const& a, fs::path const& b) { return a.filename().string() < b.filename().string(); });

        SystemOneDialect const& d = *this->dialect;
        for (fs::path const& path : files)
        {
            std::string text;
            BrainJsonValue root;
            std::string error;
            if (!ReadFile(path, text) || !BrainJson::Parse(text, root, error, MaxFixtureBytes) || !root.IsObject())
            {
                LOG_WARN("playerbots.brain", "CoA brain: mock engine {}: fixture {} skipped (unreadable, over 64 KiB "
                                             "or not JSON)", this->config.name, path.filename().string());
                continue;
            }

            BrainJsonValue const* response = root.Find("response");
            if (!response)
                continue;

            Fixture fixture;
            fixture.name = path.stem().string();
            double const status = root.GetNumber("status", 200.0);
            fixture.status = std::isfinite(status) && status >= 100.0 && status <= 599.0 ? int32(status) : 200;

            // A 200 fixture that documents a failure (expect.error present and not "none") would only feed the soak
            // broken answers: it serves the self-test, never a 200 reply of the mock. Error statuses stay, for
            // FailStatus.
            if (fixture.status == 200)
            {
                BrainJsonValue const* expect = root.Find("expect");
                BrainJsonValue const* expected = expect ? expect->Find("error") : nullptr;
                if (expected && !expected->IsNull() && !(expected->IsString() && expected->string == "none"))
                    continue;
            }

            fixture.responseBody = BrainJson::Serialize(*response);

            BrainJsonValue const* request = root.Find("request");
            BrainJsonValue const* questions = request ? request->Find(d.fQuestions) : nullptr;
            if (questions && questions->IsObject())
            {
                for (BrainJsonMember const& q : questions->members)
                {
                    fixture.questionNames.push_back(q.key);
                    fixture.questionTypes.push_back(q.value.GetString(d.fType));
                }
            }
            fixtures.push_back(std::move(fixture));
        }

        LOG_DEBUG("playerbots.brain", "CoA brain: mock engine {}: {} fixtures from {}", this->config.name,
                  fixtures.size(), fixtureDir);
    }
    catch (std::exception const& e)
    {
        LOG_WARN("playerbots.brain", "CoA brain: mock engine {}: fixtures not read ({})", this->config.name, e.what());
    }
}

DecisionReply MockDecisionEngine::Decide(DecisionCall const& call, BrainHttpClient& /*http*/, uint32 budgetMs)
{
    DecisionReply reply;
    reply.calibrated = config.calibrated;
    uint64 const start = CoaTime::NowMs();

    try
    {
        if (!call.job || !dialect)
        {
            reply.error = BrainError::BadRequest;
            reply.detail = !call.job ? "no job" : "no dialect";
            reply.ms = uint32(CoaTime::NowMs() - start);
            return reply;
        }
        SystemOneDialect const& d = *dialect;

        // Latency, in slices so a shutdown never waits for it; past the budget the call times out there.
        uint64 const wakeMs = start + std::min(config.mockLatencyMs, budgetMs);
        for (uint64 now = CoaTime::NowMs(); now < wakeMs; now = CoaTime::NowMs())
        {
            if (call.cancel && call.cancel->load())
            {
                reply.error = BrainError::Cancelled;
                reply.detail = "shutting down";
                reply.ms = uint32(CoaTime::NowMs() - start);
                return reply;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64>(SliceMs, wakeMs - now)));
        }
        if (call.cancel && call.cancel->load())
        {
            reply.error = BrainError::Cancelled;
            reply.detail = "shutting down";
            reply.ms = uint32(CoaTime::NowMs() - start);
            return reply;
        }
        if (config.mockLatencyMs > budgetMs)
        {
            reply.error = BrainError::Timeout;
            reply.detail = "mock latency " + std::to_string(config.mockLatencyMs) + " ms is over the call's " +
                           std::to_string(budgetMs) + " ms";
            reply.ms = uint32(CoaTime::NowMs() - start);
            return reply;
        }

        std::string const requestBody = call.record ? SystemOneWire::BuildRequest(d, config.model, call) : "";

        // Failure, as a provider would send it.
        if (config.mockErrorPercent && Below(100) < config.mockErrorPercent)
        {
            int32 const status = config.mockFailStatus ? int32(config.mockFailStatus) : 500;
            std::string body;
            for (Fixture const& f : fixtures)
            {
                if (f.status == status)
                {
                    body = f.responseBody;
                    break;
                }
            }
            if (body.empty())
            {
                BrainJsonValue error = BrainJsonValue::MakeObject();
                error.Set(d.fErrorCode, BrainJsonValue::MakeNumber(double(status)));
                error.Set(d.fErrorMessage, BrainJsonValue::MakeString("mock failure"));
                BrainJsonValue root = BrainJsonValue::MakeObject();
                root.Set(d.fError, std::move(error));
                body = BrainJson::Serialize(root);
            }

            reply.httpStatus = status;
            reply.error = SystemOneWire::ClassifyStatus(d, status, body, reply.detail);
            if (reply.error == BrainError::None)
                reply.error = BrainError::ServerError;
            if (call.record)
            {
                reply.url = "mock:";
                reply.rawRequest = requestBody;
                reply.rawResponse = body;
            }
            reply.ms = uint32(CoaTime::NowMs() - start);
            return reply;
        }

        // The 200 fixture sharing the most (name, wire type) pairs with the job; ties go to the first by name.
        BrainJob const& job = *call.job;
        Fixture const* best = nullptr;
        uint32 bestShared = 0;
        for (Fixture const& f : fixtures)
        {
            if (f.status != 200)
                continue;
            uint32 shared = 0;
            for (BrainQuestion const& q : job.questions)
                for (std::size_t i = 0; i < f.questionNames.size(); ++i)
                    if (f.questionNames[i] == q.name && f.questionTypes[i] == WireType(d, q.type))
                        ++shared;
            if (shared > bestShared)
            {
                best = &f;
                bestShared = shared;
            }
        }

        BrainJsonValue fixtureRoot;
        BrainJsonValue const* fixtureAnswers = nullptr;
        if (best)
        {
            std::string ignored;
            if (BrainJson::Parse(best->responseBody, fixtureRoot, ignored, SystemOneWire::MaxResponseBytes))
                fixtureAnswers = fixtureRoot.Find(d.fAnswers);
        }

        BrainJsonValue answers = BrainJsonValue::MakeObject();
        for (BrainQuestion const& q : job.questions)
        {
            BrainJsonValue const* from = fixtureAnswers ? fixtureAnswers->Find(q.name) : nullptr;
            BrainJsonValue const* type = from ? from->Find(d.fType) : nullptr;
            bool const sameType = from && from->IsObject() && (!type || (type->IsString() &&
                                                                         type->string == WireType(d, q.type)));
            answers.Set(q.name, sameType ? FitAnswer(d, q, *from) : RandomAnswer(d, q));
        }

        std::string const state = SystemOneWire::StateAsSent(call);
        BrainJsonValue usage = BrainJsonValue::MakeObject();
        usage.Set(d.fInputTokens, BrainJsonValue::MakeNumber(double(state.size() / 4)));
        usage.Set(d.fOutputTokens, BrainJsonValue::MakeNumber(0.0));
        usage.Set(d.fCost, BrainJsonValue::MakeNumber(double(config.mockUsdMicrosPerCall) / 1e6));

        std::string model = fixtureRoot.GetString(d.fResponseModel);
        BrainJsonValue root = BrainJsonValue::MakeObject();
        root.Set(d.fResponseModel, BrainJsonValue::MakeString(model.empty() ? config.model : model));
        root.Set(d.fAnswers, std::move(answers));
        root.Set(d.fUsage, std::move(usage));
        std::string const body = BrainJson::Serialize(root);

        reply.httpStatus = 200;
        SystemOneWire::ParseResponse(d, job, body, config.calibrated, config.uncalibratedConfidence, reply);
        if (reply.error != BrainError::None)
            reply.usage = BrainUsage();

        if (call.record)
        {
            reply.url = "mock:" + (best ? best->name : std::string("random"));
            reply.rawRequest = requestBody;
            reply.rawResponse = body;
        }
    }
    catch (std::exception const& e)
    {
        reply = DecisionReply();
        reply.calibrated = config.calibrated;
        reply.error = BrainError::Network;
        reply.detail = std::string("exception: ") + e.what();
        CoaText::CutUtf8Bytes(reply.detail, 300);
    }
    catch (...)
    {
        reply = DecisionReply();
        reply.calibrated = config.calibrated;
        reply.error = BrainError::Network;
        reply.detail = "exception";
    }

    if (reply.error != BrainError::None)
        reply.answers.clear();
    reply.ms = uint32(CoaTime::NowMs() - start);
    return reply;
}
