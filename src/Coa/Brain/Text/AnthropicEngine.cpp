/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AnthropicEngine.h"
#include "BrainHttpClient.h"
#include "BrainResolver.h"
#include "CoaTime.h"
#include "TextWire.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

AnthropicEngine::AnthropicEngine(BrainTextConfig config, std::shared_ptr<BrainResolver> resolver, BrainUrl url)
    : config(std::move(config)), resolver(std::move(resolver)), url(std::move(url)),
      caps(TextModelCaps::For(this->config.model)), apiKey(this->config.apiKey)
{
}

TextReply AnthropicEngine::Generate(TextCall const& call, BrainHttpClient& http, uint32 budgetMs)
{
    TextReply reply;
    uint64 const start = CoaTime::NowMs();
    auto cancelled = [&]() { return call.cancel && call.cancel->load(std::memory_order_relaxed); };
    try
    {
        if (!call.job)
        {
            reply.error = BrainError::BadRequest;
            reply.detail = "no job";
        }
        else if (cancelled())
        {
            reply.error = BrainError::Cancelled;
            reply.detail = "shutting down";
        }
        else if (!budgetMs)
        {
            reply.error = BrainError::Timeout;
            reply.detail = "no time left";
        }
        else
        {
            std::vector<std::string> addresses;
            if (url.hostIsIp)
                addresses.push_back(url.host);
            else if (url.host == "localhost")
                addresses.push_back("127.0.0.1");
            else if (resolver)
                addresses = resolver->Lookup(url.host);
            if (addresses.empty())
            {
                if (resolver)
                    resolver->RequestRefresh(url.host);
                reply.error = BrainError::Dns;
                reply.detail = "no address for " + url.host + " yet";
            }
            else
            {
                TextWire::Prompt const prompt{call.job->textSystem, call.job->textStableBytes,
                                              call.job->textUser, call.maxTokens};
                std::string const body =
                    TextWire::BuildAnthropicRequest(config, caps, prompt);
                if (body.empty())
                {
                    reply.error = BrainError::BadRequest;
                    reply.detail = "request construction failed";
                }
                else
                {
                    BrainHttpRequest request;
                    request.url = &url;
                    request.address = addresses.front();
                    request.authHeader = "x-api-key";
                    request.authorization = apiKey;
                    request.body = body;
                    request.extraHeaders = TextWire::AnthropicHeaders(config, caps);
                    request.connectTimeoutMs = std::min(call.connectTimeoutMs, budgetMs);
                    request.totalTimeoutMs = budgetMs;
                    BrainHttpResponse response = http.Post(request);
                    if (response.transport == BrainError::Network && !response.sent && addresses.size() > 1 &&
                        !cancelled())
                    {
                        uint64 const spent = CoaTime::NowMs() - start;
                        if (spent + 300 <= budgetMs)
                        {
                            request.address = addresses[1];
                            request.totalTimeoutMs = budgetMs - uint32(spent);
                            request.connectTimeoutMs = std::min(call.connectTimeoutMs, request.totalTimeoutMs);
                            response = http.Post(request);
                        }
                    }
                    reply.httpStatus = response.status;
                    if (response.transport != BrainError::None)
                    {
                        reply.error = response.transport;
                        reply.detail = response.detail;
                        if (resolver && (response.transport == BrainError::Network ||
                                         response.transport == BrainError::Timeout))
                            resolver->RequestRefresh(url.host);
                    }
                    else if (response.status == 200)
                        TextWire::ParseAnthropicResponse(config, response.body, reply);
                    else
                        reply.error =
                            TextWire::ClassifyAnthropicStatus(response.status, response.body, reply.detail);
                    if (response.status != 200)
                        reply.usage = {};
                    else if (!reply.usage.costReported && !reply.usage.inputTokens && !reply.usage.outputTokens)
                    {
                        long double const estimate = std::ceil(
                            static_cast<long double>(body.size()) / 3.5L * config.priceInPerMTok - 1e-9L);
                        if (std::isfinite(estimate) && estimate > 0)
                            reply.usage.usdMicros = estimate >= std::numeric_limits<uint64>::max()
                                ? std::numeric_limits<uint64>::max() : uint64(estimate);
                    }
                    if (call.record)
                    {
                        reply.url = config.url;
                        reply.rawRequest = body;
                        reply.rawResponse = std::move(response.body);
                    }
                }
            }
        }
        if (!config.apiKey.empty())
        {
            std::size_t at = 0;
            while ((at = reply.detail.find(config.apiKey, at)) != std::string::npos)
            {
                reply.detail.replace(at, config.apiKey.size(), "[redacted]");
                at += 10;
            }
        }
        if (cancelled())
            reply.error = BrainError::Cancelled;
    }
    catch (std::exception const&)
    {
        reply = {};
        reply.error = BrainError::Network;
        reply.detail = "exception in text transport";
    }
    catch (...)
    {
        reply = {};
        reply.error = BrainError::Network;
        reply.detail = "exception in text transport";
    }
    if (reply.error != BrainError::None)
        reply.text.clear();
    reply.ms = uint32(CoaTime::NowMs() - start);
    return reply;
}
