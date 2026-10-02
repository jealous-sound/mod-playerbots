/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ANTHROPICENGINE_H
#define PLAYERBOTS_ANTHROPICENGINE_H

#include "BrainHttpClient.h"
#include "ITextEngine.h"
#include "TextModelCaps.h"
#include <memory>
#include <string>

// Text.Provider = anthropic: raw HTTP POST to Text.Url (https://api.anthropic.com/v1/messages) with the headers
// x-api-key, anthropic-version: 2023-06-01 and content-type application/json (and anthropic-beta with the
// fallbacks), the body of TextWire::BuildAnthropicRequest for the model's TextModelCaps. There is no official C++
// SDK. Generate always reads stop_reason: a "refusal", or "max_tokens" without text, is Refused, so the director
// sends a template. Same address, retry and cost rules as OpenAiChatEngine (cache tokens priced as TextWire says).
class AnthropicEngine final : public ITextEngine
{
public:
    AnthropicEngine(BrainTextConfig config, std::shared_ptr<BrainResolver> resolver, BrainUrl url);

    BrainTextConfig const& Config() const override { return config; }
    TextReply Generate(TextCall const& call, BrainHttpClient& http, uint32 budgetMs) override;

    TextModelCaps const& Caps() const { return caps; }

private:
    BrainTextConfig const config;
    std::shared_ptr<BrainResolver> const resolver;  // null for loopback and IP-literal URLs (tests)
    BrainUrl const url;
    TextModelCaps const caps;         // TextModelCaps::For(config.model)
    std::string const apiKey;         // the x-api-key value; never logged
};

#endif
