/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_OPENAICHATENGINE_H
#define PLAYERBOTS_OPENAICHATENGINE_H

#include "BrainHttpClient.h"
#include "ITextEngine.h"
#include <memory>
#include <string>

// Text.Provider = openai: POST {Text.Url} with an OpenAI chat-completions body (TextWire::BuildOpenAiRequest),
// Authorization: Bearer <key> when a key is set (optional for a loopback URL). Covers Ollama
// (/v1/chat/completions), llama.cpp, vLLM, LM Studio and OpenRouter, DeepSeek, Groq, Together. Generate: the
// address from the resolver (or the URL's IP literal, or localhost -> 127.0.0.1; none -> Dns and a refresh request),
// one POST through the worker's client, then ParseOpenAiResponse or ClassifyOpenAiStatus. A Network failure before
// the request was sent, with another address and time left, tries the next address once; a request that may have
// reached the server is never sent again. Cost: usage.cost when present, else tokens x price.
class OpenAiChatEngine final : public ITextEngine
{
public:
    OpenAiChatEngine(BrainTextConfig config, std::shared_ptr<BrainResolver> resolver, BrainUrl url);

    BrainTextConfig const& Config() const override { return config; }
    TextReply Generate(TextCall const& call, BrainHttpClient& http, uint32 budgetMs) override;

private:
    BrainTextConfig const config;
    std::shared_ptr<BrainResolver> const resolver;  // null for loopback and IP-literal URLs
    BrainUrl const url;
    std::string const authorization;  // "Bearer <key>", or empty; never logged
};

#endif
