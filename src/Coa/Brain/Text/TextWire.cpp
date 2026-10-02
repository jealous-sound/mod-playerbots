/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TextWire.h"
#include "BrainJson.h"
#include "CoaText.h"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <exception>
#include <limits>

namespace
{
using Json = BrainJsonValue;

uint64 CeilMicros(long double value)
{
    if (!std::isfinite(value) || value <= 0)
        return 0;
    value = std::ceil(value - 1e-9L);
    return value >= std::numeric_limits<uint64>::max() ? std::numeric_limits<uint64>::max() : uint64(value);
}

double Temperature(float value)
{
    char buffer[32];
    auto const result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    double decimal = value;
    if (result.ec == std::errc())
        std::from_chars(buffer, result.ptr, decimal);
    return decimal;
}

uint32 Tokens(Json const& value, char const* key)
{
    double const n = value.GetNumber(key);
    if (!std::isfinite(n) || n <= 0)
        return 0;
    return n >= std::numeric_limits<uint32>::max() ? std::numeric_limits<uint32>::max() : uint32(n);
}

uint32 SumTokens(uint32 a, uint32 b, uint32 c = 0)
{
    return uint32(std::min<uint64>(uint64(a) + b + c, std::numeric_limits<uint32>::max()));
}

void Cut(std::string& value, std::size_t cap)
{
    CoaText::CutUtf8Bytes(value, cap);
}

void TrimText(std::string& value)
{
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        value.clear();
    else
        value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    Cut(value, TextWire::MAX_TEXT_BYTES);
}

Json Message(char const* role, std::string_view content)
{
    Json result = Json::MakeObject();
    result.Set("role", Json::MakeString(role));
    result.Set("content", Json::MakeString(std::string(content)));
    return result;
}

std::string ErrorMessage(std::string_view body)
{
    Json root;
    std::string problem;
    std::string text;
    if (BrainJson::Parse(body, root, problem))
    {
        Json const* value = root.Find("error");
        if (value && value->IsObject())
            text = value->GetString("message");
        else if (value && value->IsString())
            text = value->string;
        if (text.empty())
            text = root.GetString("detail");
        if (text.empty())
            text = "provider error";
    }
    else
        text = std::string(body.substr(0, 300));
    Cut(text, 300);
    return text;
}

BrainError Classify(int32 status, std::string_view body, std::string& detail, bool anthropic)
{
    detail = ErrorMessage(body);
    std::string lower = detail;
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    auto has = [&](char const* word) { return lower.find(word) != std::string::npos; };
    if (status == 401 || status == 403)
        return BrainError::AuthFailed;
    if (status == 402 || status == 429)
        return BrainError::Exhausted;
    if (status == 404)
        return BrainError::Misconfigured;
    if (status == 400 || status == 413 || status == 422)
    {
        if (has("billing") || has("spend limit") ||
            (anthropic ? has("credit balance") : has("quota") || has("credit")))
            return BrainError::Exhausted;
        if (anthropic ? has("model") && (has("not found") || has("does not exist"))
                      : has("not found") || has("does not exist") || has("unknown model"))
            return BrainError::Misconfigured;
        return BrainError::BadRequest;
    }
    if (status == 408 || (status >= 500 && status <= 599))
        return BrainError::ServerError;
    return BrainError::BadRequest;
}

void Bad(TextReply& reply, char const* reason, BrainError error = BrainError::BadResponse)
{
    reply.error = error;
    reply.detail = reason;
    reply.text.clear();
}

void AnthropicUsage(BrainTextConfig const& config, Json const& usage, TextReply& reply)
{
    uint32 const input = Tokens(usage, "input_tokens");
    uint32 const output = Tokens(usage, "output_tokens");
    uint32 const read = Tokens(usage, "cache_read_input_tokens");
    uint32 const write = Tokens(usage, "cache_creation_input_tokens");
    reply.usage.inputTokens = SumTokens(input, read, write);
    reply.usage.outputTokens = output;
    reply.usage.usdMicros = TextWire::UsageMicros(config, input, output, read, write);
    // Fallback usage at the top level covers only the last attempt.
    Json const* iterations = usage.Find("iterations");
    if (iterations && iterations->IsArray() && !iterations->items.empty())
    {
        reply.usage = {};
        for (Json const& iteration : iterations->items)
        {
            BrainTextConfig prices = config;
            TextModelCaps const caps = TextModelCaps::For(iteration.GetString("model"));
            if (caps.known)
            {
                prices.priceInPerMTok = std::max(prices.priceInPerMTok, caps.priceInPerMTok);
                prices.priceOutPerMTok = std::max(prices.priceOutPerMTok, caps.priceOutPerMTok);
            }
            uint32 const in = Tokens(iteration, "input_tokens");
            uint32 const out = Tokens(iteration, "output_tokens");
            uint32 const cacheRead = Tokens(iteration, "cache_read_input_tokens");
            uint32 const cacheWrite = Tokens(iteration, "cache_creation_input_tokens");
            reply.usage.inputTokens = SumTokens(reply.usage.inputTokens, SumTokens(in, cacheRead, cacheWrite));
            reply.usage.outputTokens = SumTokens(reply.usage.outputTokens, out);
            uint64 const cost = TextWire::UsageMicros(prices, in, out, cacheRead, cacheWrite);
            reply.usage.usdMicros += std::min(cost, std::numeric_limits<uint64>::max() - reply.usage.usdMicros);
        }
    }
}
}

std::string TextWire::BuildOpenAiRequest(BrainTextConfig const& config, Prompt const& prompt)
{
    try
    {
        Json body = Json::MakeObject();
        body.Set("model", Json::MakeString(config.model));
        Json messages = Json::MakeArray();
        messages.Push(Message("system", prompt.system));
        messages.Push(Message("user", prompt.user));
        body.Set("messages", std::move(messages));
        body.Set("max_tokens", Json::MakeNumber(prompt.maxTokens));
        body.Set("stream", Json::MakeBool(false));
        if (config.hasTemperature)
            body.Set("temperature", Json::MakeNumber(Temperature(config.temperature)));
        if (!config.reasoningEffort.empty())
            body.Set("reasoning_effort", Json::MakeString(config.reasoningEffort));
        return BrainJson::Serialize(body);
    }
    catch (std::exception const&)
    {
        return {};
    }
    catch (...)
    {
        return {};
    }
}

std::string TextWire::BuildAnthropicRequest(BrainTextConfig const& config, TextModelCaps const& caps,
                                           Prompt const& prompt)
{
    try
    {
        Json body = Json::MakeObject();
        body.Set("model", Json::MakeString(config.model));
        body.Set("max_tokens", Json::MakeNumber(prompt.maxTokens));
        std::size_t const stable = std::min<std::size_t>(prompt.stableBytes, prompt.system.size());
        if (caps.known && config.anthropicPromptCache && stable)
        {
            Json blocks = Json::MakeArray();
            Json prefix = Json::MakeObject();
            prefix.Set("type", Json::MakeString("text"));
            prefix.Set("text", Json::MakeString(std::string(prompt.system.substr(0, stable))));
            Json cache = Json::MakeObject();
            cache.Set("type", Json::MakeString("ephemeral"));
            prefix.Set("cache_control", std::move(cache));
            blocks.Push(std::move(prefix));
            if (stable < prompt.system.size())
            {
                Json tail = Json::MakeObject();
                tail.Set("type", Json::MakeString("text"));
                tail.Set("text", Json::MakeString(std::string(prompt.system.substr(stable))));
                blocks.Push(std::move(tail));
            }
            body.Set("system", std::move(blocks));
        }
        else
            body.Set("system", Json::MakeString(std::string(prompt.system)));
        Json messages = Json::MakeArray();
        messages.Push(Message("user", prompt.user));
        body.Set("messages", std::move(messages));
        if (caps.effort)
        {
            Json effort = Json::MakeObject();
            effort.Set("effort", Json::MakeString(config.reasoningEffort.empty() ? "low" : config.reasoningEffort));
            body.Set("output_config", std::move(effort));
        }
        if (caps.serverFallbacks && config.anthropicServerFallbacks)
            body.Set("fallbacks", Json::MakeString("default"));
        if (caps.sampling && config.hasTemperature)
            body.Set("temperature", Json::MakeNumber(Temperature(config.temperature)));
        return BrainJson::Serialize(body);
    }
    catch (std::exception const&)
    {
        return {};
    }
    catch (...)
    {
        return {};
    }
}

std::vector<std::pair<std::string, std::string>> TextWire::AnthropicHeaders(BrainTextConfig const& config,
                                                                         TextModelCaps const& caps)
{
    try
    {
        std::vector<std::pair<std::string, std::string>> headers{{"anthropic-version", ANTHROPIC_VERSION}};
        if (caps.serverFallbacks && config.anthropicServerFallbacks)
            headers.emplace_back("anthropic-beta", ANTHROPIC_FALLBACK_BETA);
        return headers;
    }
    catch (std::exception const&)
    {
        return {};
    }
    catch (...)
    {
        return {};
    }
}

void TextWire::ParseOpenAiResponse(BrainTextConfig const& config, std::string_view body, TextReply& reply)
{
    try
    {
        reply.text.clear();
        reply.usage = {};
        reply.error = BrainError::None;
        Json root;
        std::string problem;
        if (!BrainJson::Parse(body, root, problem) || !root.IsObject())
            return Bad(reply, "not JSON");
        Json const* choices = root.Find("choices");
        if (!choices || !choices->IsArray() || choices->items.empty())
        {
            Json const* providerError = root.Find("error");
            if (providerError)
            {
                double const code = providerError->IsObject() ? providerError->GetNumber("code", 500) : 500;
                int32 const status = std::isfinite(code) && code >= 100 && code <= 599 ? int32(code) : 500;
                reply.error = ClassifyOpenAiStatus(status, body, reply.detail);
                return;
            }
            return Bad(reply, "no choices");
        }
        Json const& choice = choices->items.front();
        Json const* message = choice.Find("message");
        if (!message || !message->IsObject())
            return Bad(reply, "no message");
        reply.providerModel = root.GetString("model");
        reply.finishReason = choice.GetString("finish_reason");
        Json const* content = message->Find("content");
        Json const* refusal = message->Find("refusal");
        if (content && content->IsString())
            reply.text = content->string;
        else if (content && content->IsArray())
        {
            for (Json const& part : content->items)
                if (part.GetString("type") == "text")
                    reply.text += part.GetString("text");
        }
        else if (!refusal || refusal->IsNull())
            return Bad(reply, "invalid content");
        TrimText(reply.text);
        if (Json const* usage = root.Find("usage"); usage && usage->IsObject())
        {
            reply.usage.inputTokens = Tokens(*usage, "prompt_tokens");
            reply.usage.outputTokens = Tokens(*usage, "completion_tokens");
            Json const* cost = usage->Find("cost");
            reply.usage.costReported = cost && cost->IsNumber();
            reply.usage.usdMicros = reply.usage.costReported ? CeilMicros(cost->number * 1e6L)
                : UsageMicros(config, reply.usage.inputTokens, reply.usage.outputTokens);
        }
        if (refusal && !refusal->IsNull())
            return Bad(reply, "refusal", BrainError::Refused);
        if (reply.finishReason == "content_filter")
            return Bad(reply, "content filter", BrainError::Refused);
        if (reply.text.empty())
            return Bad(reply, "empty content", BrainError::Refused);
    }
    catch (std::exception const&)
    {
        Bad(reply, "parse exception");
    }
    catch (...)
    {
        Bad(reply, "parse exception");
    }
}

void TextWire::ParseAnthropicResponse(BrainTextConfig const& config, std::string_view body, TextReply& reply)
{
    try
    {
        reply.text.clear();
        reply.usage = {};
        reply.error = BrainError::None;
        Json root;
        std::string problem;
        if (!BrainJson::Parse(body, root, problem) || !root.IsObject())
            return Bad(reply, "not JSON");
        Json const* content = root.Find("content");
        if (!content || !content->IsArray())
            return Bad(reply, "invalid content");
        for (Json const& block : content->items)
            if (block.GetString("type") == "text")
                reply.text += block.GetString("text");
        TrimText(reply.text);
        reply.providerModel = root.GetString("model");
        reply.finishReason = root.GetString("stop_reason");
        if (Json const* usage = root.Find("usage"); usage && usage->IsObject())
            AnthropicUsage(config, *usage, reply);
        if (reply.finishReason == "refusal")
            return Bad(reply, "refusal", BrainError::Refused);
        if (reply.finishReason != "end_turn" && reply.finishReason != "stop_sequence" &&
            reply.finishReason != "max_tokens")
            return Bad(reply, "unexpected stop reason");
        if (reply.text.empty())
            return Bad(reply, "empty content", BrainError::Refused);
    }
    catch (std::exception const&)
    {
        Bad(reply, "parse exception");
    }
    catch (...)
    {
        Bad(reply, "parse exception");
    }
}

BrainError TextWire::ClassifyOpenAiStatus(int32 status, std::string_view body, std::string& detail)
{
    try
    {
        return Classify(status, body, detail, false);
    }
    catch (std::exception const&)
    {
        detail = "status exception"; return BrainError::BadResponse;
    }
    catch (...)
    {
        detail = "status exception"; return BrainError::BadResponse;
    }
}

BrainError TextWire::ClassifyAnthropicStatus(int32 status, std::string_view body, std::string& detail)
{
    try
    {
        return Classify(status, body, detail, true);
    }
    catch (std::exception const&)
    {
        detail = "status exception"; return BrainError::BadResponse;
    }
    catch (...)
    {
        detail = "status exception"; return BrainError::BadResponse;
    }
}

uint64 TextWire::UsageMicros(BrainTextConfig const& config, uint32 inputTokens, uint32 outputTokens,
                            uint32 cacheReadTokens, uint32 cacheWriteTokens)
{
    return CeilMicros(static_cast<long double>(inputTokens) * config.priceInPerMTok +
                      static_cast<long double>(outputTokens) * config.priceOutPerMTok +
                      static_cast<long double>(cacheReadTokens) * config.priceInPerMTok * 0.1L +
                      static_cast<long double>(cacheWriteTokens) * config.priceInPerMTok * 1.25L);
}

uint64 TextWire::EstimateMicros(BrainTextConfig const& config, std::size_t promptBytes, uint32 maxTokens)
{
    if (config.provider == BrainTextProvider::Mock && config.mockUsdMicrosPerCall)
        return config.mockUsdMicrosPerCall;
    long double value = static_cast<long double>(promptBytes) / 3.5L * config.priceInPerMTok +
                        static_cast<long double>(maxTokens) * config.priceOutPerMTok;
    if (config.provider == BrainTextProvider::Anthropic)
    {
        // A cache miss may write the prompt; server fallbacks can bill more than one attempt.
        if (config.anthropicPromptCache)
            value += static_cast<long double>(promptBytes) / 3.5L * config.priceInPerMTok * 0.25L;
        if (config.anthropicServerFallbacks && TextModelCaps::For(config.model).serverFallbacks)
            value *= 4;
    }
    return std::max<uint64>(1, CeilMicros(value));
}
