/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "JevEngine.h"
#include "BrainResolver.h"
#include "CoaText.h"
#include "CoaTime.h"
#include "SystemOneDialects.h"
#include "SystemOneWire.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace
{
constexpr uint32 SecondAddressMinMs = 300;
constexpr double BytesPerToken = 3.5;

// Tokens x USD per million tokens is micro-USD. Rounded up, with a hair of slack for binary fractions.
uint64 CeilMicros(double micros)
{
    if (!std::isfinite(micros) || micros <= 0.0)
        return 0;
    double const up = std::ceil(micros - 1e-6);
    if (up >= 1e15)
        return uint64(1e15);
    return up > 0.0 ? uint64(up) : 0;
}

bool Cancelled(DecisionCall const& call) { return call.cancel && call.cancel->load(); }
}  // namespace

JevEngine::JevEngine(BrainEngineConfig config, std::shared_ptr<SystemOneDialect const> dialect,
                     std::shared_ptr<BrainResolver> resolver, BrainUrl url)
    : config(std::move(config)),
      dialect(std::move(dialect)),
      resolver(std::move(resolver)),
      url(std::move(url)),
      authorization(this->config.apiKey.empty() || !this->dialect ? std::string()
                                                                  : this->dialect->authPrefix + this->config.apiKey)
{
}

DecisionReply JevEngine::Decide(DecisionCall const& call, BrainHttpClient& http, uint32 budgetMs)
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
        }
        else if (Cancelled(call))
        {
            reply.error = BrainError::Cancelled;
            reply.detail = "shutting down";
        }
        else
        {
            // 1. An address: the URL's own literal, loopback for "localhost", else the resolver's cache. The name
            // is never resolved here: getaddrinfo cannot be bounded, so it runs on the resolver thread only.
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
                // 2-3. The body, one POST, and a second address once when the first could not be reached (connect
                // or TLS handshake failed, so the request never left) and time is left. Once the request may have
                // reached the server it is never sent again: that call could be billed twice for one reservation.
                std::string const body = SystemOneWire::BuildRequest(*dialect, config.model, call);

                BrainHttpRequest request;
                request.url = &url;
                request.address = addresses[0];
                request.authHeader = dialect->authHeader;
                request.authorization = authorization;
                request.body = body;
                request.connectTimeoutMs = std::min(call.connectTimeoutMs, budgetMs);
                request.totalTimeoutMs = budgetMs;

                BrainHttpResponse response = http.Post(request);
                if (response.transport == BrainError::Network && !response.sent && addresses.size() > 1 &&
                    !Cancelled(call))
                {
                    uint64 const spent = CoaTime::NowMs() - start;
                    if (spent + SecondAddressMinMs <= budgetMs)
                    {
                        request.address = addresses[1];
                        request.totalTimeoutMs = budgetMs - uint32(spent);
                        request.connectTimeoutMs = std::min(call.connectTimeoutMs, request.totalTimeoutMs);
                        response = http.Post(request);
                    }
                }

                // 4. Transport failure, answer, or an HTTP error.
                if (response.transport != BrainError::None)
                {
                    reply.error = response.transport;
                    reply.detail = response.detail;
                    if (resolver && (response.transport == BrainError::Network ||
                                     response.transport == BrainError::Timeout))
                        resolver->RequestRefresh(url.host);
                }
                else
                {
                    reply.httpStatus = response.status;
                    if (response.status == 200)
                        SystemOneWire::ParseResponse(*dialect, *call.job, response.body, config.calibrated,
                                                     config.uncalibratedConfidence, reply);
                    else
                        reply.error = SystemOneWire::ClassifyStatus(*dialect, response.status, response.body,
                                                                    reply.detail);
                }

                // 5. Cost: the provider's figure, else tokens x price, else the request's bytes x price. Errors
                // carry none, except a 200 whose answers were unusable but whose usage was reported: that call
                // was billed, and the budget must see it.
                bool const tokensKnown = reply.usage.inputTokens || reply.usage.outputTokens;
                bool const billed = reply.error == BrainError::None ||
                                    (reply.httpStatus == 200 && (reply.usage.costReported || tokensKnown));
                if (!billed)
                    reply.usage = BrainUsage();
                else if (!reply.usage.costReported)
                {
                    if (tokensKnown)
                        reply.usage.usdMicros = CeilMicros(double(reply.usage.inputTokens) * config.priceInPerMTok +
                                                           double(reply.usage.outputTokens) * config.priceOutPerMTok);
                    else
                        reply.usage.usdMicros = CeilMicros(double(body.size()) / BytesPerToken * config.priceInPerMTok);
                }

                // 6. Probe record: the bodies only (the key travels in a header, never in them).
                if (call.record)
                {
                    reply.url = config.url;
                    reply.rawRequest = body;
                    reply.rawResponse = std::move(response.body);
                }
            }
        }

        // 7. A failure during shutdown is a cancellation, whatever the socket said. An answer that made it stays
        // an answer, so its cost is counted.
        if (reply.error != BrainError::None && Cancelled(call))
            reply.error = BrainError::Cancelled;
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
