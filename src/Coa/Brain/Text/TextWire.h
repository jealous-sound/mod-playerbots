/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TEXTWIRE_H
#define PLAYERBOTS_TEXTWIRE_H

#include "BrainConfig.h"
#include "ITextEngine.h"
#include "TextModelCaps.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The exact JSON of the two providers (contract, section 5): request builders, response parsers, status
// classification and prices. Pure functions, any thread, no network, no exception. BrainJson is the only JSON
// code; the member order of every request is fixed so a request is byte-stable.
namespace TextWire
{
constexpr char const* ANTHROPIC_VERSION = "2023-06-01";                   // the anthropic-version header
constexpr char const* ANTHROPIC_FALLBACK_BETA = "server-side-fallback-2026-07-01";  // the anthropic-beta header
constexpr std::size_t MAX_TEXT_BYTES = 4096;  // a longer model text is cut (UTF-8 boundary) when parsed

// One prompt as the engines send it. `stableBytes`: the first bytes of `system` that never change from call to call.
struct Prompt
{
    std::string_view system;
    uint32 stableBytes = 0;
    std::string_view user;
    uint32 maxTokens = 1024;
};

// OpenAI-compatible body: model, messages [system, user], max_tokens, stream false, then temperature (when
// Text.Temperature is set) and reasoning_effort (when Text.ReasoningEffort is set).
std::string BuildOpenAiRequest(BrainTextConfig const& config, Prompt const& prompt);

// Anthropic body: model, max_tokens, system, messages [user], then output_config {"effort"} (caps.effort),
// fallbacks "default" (caps.serverFallbacks and Text.Anthropic.ServerFallbacks), temperature (caps.sampling and
// Text.Temperature). `system` is one string, or (caps.known, Text.Anthropic.PromptCache, stableBytes > 0) an array of
// two text blocks, the stable prefix with cache_control {"type":"ephemeral"} first, the rest second.
std::string BuildAnthropicRequest(BrainTextConfig const& config, TextModelCaps const& caps, Prompt const& prompt);

// The headers beyond x-api-key and Content-Type: anthropic-version always, anthropic-beta with the fallbacks.
std::vector<std::pair<std::string, std::string>> AnthropicHeaders(BrainTextConfig const& config,
                                                                  TextModelCaps const& caps);

// A 200 body. Fills `reply`: error None with text, finishReason, usage, providerModel; or Refused (a refusal, a
// content filter, or no text at all: cut off by max_tokens / length, empty content); or BadResponse (not the
// provider's shape, a stop_reason this request cannot produce). `reply.detail` says which, in a few words.
// OpenAI: choices[0].message.content (a string, or an array of {"type":"text","text"} parts), finish_reason,
// message.refusal, usage prompt_tokens / completion_tokens / cost. Anthropic: the content[] blocks of type "text"
// (others are skipped), stop_reason (ALWAYS checked: "refusal" is Refused, "max_tokens" without text is Refused),
// usage input_tokens / output_tokens / cache_creation_input_tokens / cache_read_input_tokens.
void ParseOpenAiResponse(BrainTextConfig const& config, std::string_view body, TextReply& reply);
void ParseAnthropicResponse(BrainTextConfig const& config, std::string_view body, TextReply& reply);

// A non-200 status: Exhausted (402, 429, a spend-limit or quota text), AuthFailed (401, 403), Misconfigured (404 or
// an unknown-model text), BadRequest (400, 413, 422), else ServerError (408, 5xx, 524, 529). `detail` gets the
// provider's error message, cut to 300 bytes.
BrainError ClassifyOpenAiStatus(int32 status, std::string_view body, std::string& detail);
BrainError ClassifyAnthropicStatus(int32 status, std::string_view body, std::string& detail);

// Micro-USD of a call from its tokens: input x PriceInPerMTok + output x PriceOutPerMTok; for Anthropic also
// cache reads at 0.1 x and cache writes at 1.25 x the input price. Rounded up.
uint64 UsageMicros(BrainTextConfig const& config, uint32 inputTokens, uint32 outputTokens, uint32 cacheReadTokens = 0,
                   uint32 cacheWriteTokens = 0);

// What Submit reserves for one text job: the prompt bytes as tokens (3.5 bytes each) at the input price plus
// maxTokens at the output price (a mock: its UsdPerCall), at least 1.
uint64 EstimateMicros(BrainTextConfig const& config, std::size_t promptBytes, uint32 maxTokens);
}  // namespace TextWire

#endif
