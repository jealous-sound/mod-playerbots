/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ITEXTENGINE_H
#define PLAYERBOTS_ITEXTENGINE_H

#include "BrainConfig.h"
#include "BrainTypes.h"
#include <atomic>
#include <memory>
#include <string>

class BrainHttpClient;
class BrainResolver;

// One call to the text engine, as a text worker hands it over. Everything is borrowed for the duration of Generate.
struct TextCall
{
    BrainJob const* job = nullptr;  // kind Text: textSystem, textStableBytes, textUser
    uint32 maxTokens = 1024;        // the job's textMaxTokens, else CoaBrain.Text.MaxTokens
    uint32 connectTimeoutMs = 2000;
    bool record = false;                        // keep the raw bodies in the reply (text probe)
    std::atomic<bool> const* cancel = nullptr;  // raised at shutdown: stop waiting, return Cancelled
};

struct TextReply
{
    BrainError error = BrainError::None;  // None, or Refused (refusal / no text), BadResponse, the HTTP classes...
    int32 httpStatus = 0;
    std::string text;           // error None: the model's text, surrounding whitespace trimmed, at most 4096 bytes
    std::string finishReason;   // finish_reason / stop_reason as sent, "" when absent
    BrainUsage usage;           // tokens, and usd_micros: the reported cost, else tokens x price
    uint32 ms = 0;
    std::string providerModel;  // the response's "model"
    std::string detail;         // error excerpt, at most 300 bytes, never a key or a header
    std::string url;            // record only
    std::string rawRequest;     // record only
    std::string rawResponse;    // record only
};

// The text generator: turns one system prompt and one user message into one short line. Built on the world thread
// and then shared by every text worker, so Generate must be thread-safe: an engine holds only immutable
// configuration, and per-call state lives on the stack and in `http`, the calling worker's own client. Never touches
// a game object; never logs a key, a prompt or a generated line.
class ITextEngine
{
public:
    virtual ~ITextEngine() = default;

    virtual BrainTextConfig const& Config() const = 0;

    // Text worker thread. At most budgetMs of wall time (connect, TLS, write, read); honours call.cancel. Always
    // returns: a throw inside becomes a reply with error Network and detail "exception: ...".
    virtual TextReply Generate(TextCall const& call, BrainHttpClient& http, uint32 budgetMs) = 0;
};

namespace TextEngines
{
// World thread (InProcessBrain::Start/Reconfigure). OpenAiChatEngine for openai, AnthropicEngine for anthropic,
// MockTextEngine for mock. Null, with `error`, when config.usable is false or the URL does not parse. Implemented in
// TextEngines.cpp.
std::shared_ptr<ITextEngine> Create(BrainTextConfig const& config, BrainConfig const& brain,
                                    std::shared_ptr<BrainResolver> const& resolver, std::string& error);
}  // namespace TextEngines

#endif
