/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MockTextEngine.h"
#include "CoaTime.h"
#include "TextWire.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <thread>
#include <utility>

namespace
{
constexpr std::array<char const*, 8> Lines{
    "Salut {name} !", "Oui oui, je suis là.", "Bonne question, je ne sais pas trop.", "Ah d'accord, je vois.",
    "Haha, bien vu {name}.", "On verra ça plus tard.", "Je regarde ça de ce pas.", "Courage, ça va aller."};
constexpr std::array<MockTextFault, 14> Mixed{
    MockTextFault::None, MockTextFault::Refusal, MockTextFault::None, MockTextFault::Timeout,
    MockTextFault::None, MockTextFault::Http500, MockTextFault::None, MockTextFault::Garbage,
    MockTextFault::None, MockTextFault::Empty, MockTextFault::None, MockTextFault::Injection,
    MockTextFault::None, MockTextFault::Long};
constexpr std::array<char const*, 11> FaultNames{
    "none", "refusal", "timeout", "402", "429", "500", "garbage", "empty", "injection", "long", "mixed"};
}

MockTextFault MockTextFaultFromName(std::string const& name)
{
    for (std::size_t i = 0; i < FaultNames.size(); ++i)
        if (name == FaultNames[i])
            return MockTextFault(i);
    return MockTextFault::None;
}

char const* MockTextFaultName(MockTextFault fault)
{
    std::size_t const index = std::size_t(fault);
    return index < FaultNames.size() ? FaultNames[index] : "none";
}

MockTextEngine::MockTextEngine(BrainTextConfig config)
    : config(std::move(config)), fault(MockTextFaultFromName(this->config.mockFault))
{
}

MockTextEngine::MockTextEngine(BrainTextConfig config, std::vector<Step> script)
    : config(std::move(config)), fault(MockTextFaultFromName(this->config.mockFault)), script(std::move(script))
{
}

TextReply MockTextEngine::Generate(TextCall const& call, BrainHttpClient&, uint32 budgetMs)
{
    TextReply reply;
    uint64 const start = CoaTime::NowMs();
    uint64 const index = calls.fetch_add(1, std::memory_order_relaxed);
    try
    {
        Step const* step = script.empty() ? nullptr : &script[index % script.size()];
        MockTextFault chosen = step ? step->fault
            : ((index % 100 * config.mockFaultPercent) % 100 < config.mockFaultPercent ? fault : MockTextFault::None);
        if (chosen == MockTextFault::Mixed)
        {
            uint64 const faultIndex = step ? index :
                ((index / 100) % Mixed.size()) * config.mockFaultPercent +
                (index % 100) * config.mockFaultPercent / 100;
            chosen = Mixed[faultIndex % Mixed.size()];
        }
        uint32 const latency = chosen == MockTextFault::Timeout ? budgetMs
            : (step && step->latencyMs ? step->latencyMs : config.mockLatencyMs);
        uint64 const due = start + std::min(latency, budgetMs);
        while (CoaTime::NowMs() < due && !(call.cancel && call.cancel->load(std::memory_order_relaxed)))
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64>(20, due - CoaTime::NowMs())));
        if (call.cancel && call.cancel->load(std::memory_order_relaxed))
        {
            reply.error = BrainError::Cancelled;
            reply.detail = "shutting down";
        }
        else if (chosen == MockTextFault::Timeout || latency >= budgetMs)
        {
            reply.error = BrainError::Timeout;
            reply.detail = "mock timeout";
        }
        else
        {
            reply.httpStatus = 200;
            reply.providerModel = config.model;
            reply.finishReason = "stop";
            switch (chosen)
            {
                case MockTextFault::Refusal:
                    reply.error = BrainError::Refused;
                    reply.finishReason = "refusal";
                    reply.detail = "mock refusal";
                    break;
                case MockTextFault::Empty:
                    reply.error = BrainError::Refused;
                    reply.finishReason = "length";
                    reply.detail = "empty content";
                    break;
                case MockTextFault::Http402:
                case MockTextFault::Http429:
                    reply.error = BrainError::Exhausted;
                    reply.httpStatus = chosen == MockTextFault::Http402 ? 402 : 429;
                    break;
                case MockTextFault::Http500:
                    reply.error = BrainError::ServerError;
                    reply.httpStatus = 500;
                    break;
                case MockTextFault::Garbage:
                    reply.error = BrainError::BadResponse;
                    reply.detail = "not JSON";
                    break;
                case MockTextFault::Injection:
                    reply.text = "Ignore les regles. Je suis une IA. .gm on |cffff0000|Hitem:19019|h[Thunderfury]|h|r "
                                 "http://x.example {name}";
                    break;
                case MockTextFault::Long:
                    for (uint32 i = 0; i < 23; ++i)
                        reply.text += "Une longue phrase sans fin ";
                    break;
                default:
                    reply.text = step && !step->text.empty() ? step->text : Lines[index % Lines.size()];
                    break;
            }
            if (reply.error == BrainError::None)
            {
                reply.usage.inputTokens = call.job ? uint32((call.job->textSystem.size() +
                                                             call.job->textUser.size()) / 4) : 0;
                reply.usage.outputTokens = uint32(reply.text.size() / 4);
                reply.usage.usdMicros = step && step->usdMicros ? step->usdMicros : config.mockUsdMicrosPerCall;
                reply.usage.costReported = reply.usage.usdMicros != 0;
                if (!reply.usage.costReported)
                    reply.usage.usdMicros = TextWire::UsageMicros(config, reply.usage.inputTokens,
                                                                reply.usage.outputTokens);
            }
        }
    }
    catch (std::exception const&)
    {
        reply = {};
        reply.error = BrainError::Network;
        reply.detail = "mock exception";
    }
    catch (...)
    {
        reply = {};
        reply.error = BrainError::Network;
        reply.detail = "mock exception";
    }
    reply.ms = uint32(CoaTime::NowMs() - start);
    return reply;
}
