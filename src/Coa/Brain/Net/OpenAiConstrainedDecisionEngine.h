/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_OPENAICONSTRAINEDDECISIONENGINE_H
#define PLAYERBOTS_OPENAICONSTRAINEDDECISIONENGINE_H

#include "IDecisionEngine.h"
#include <utility>

// Placeholder for the M2b local engine: an OpenAI-compatible /v1/chat/completions call to llama.cpp or vLLM with
// constrained decoding (json_schema or GBNF) and token logprobs. Not built in M2: every call answers NotBuilt at
// once, and BrainConfig marks such an engine unusable ("openai-constrained is not built yet (M2b)").
class OpenAiConstrainedDecisionEngine final : public IDecisionEngine
{
public:
    explicit OpenAiConstrainedDecisionEngine(BrainEngineConfig config) : config(std::move(config)) {}

    BrainEngineConfig const& Config() const override { return config; }
    bool Calibrated() const override { return false; }

    DecisionReply Decide(DecisionCall const& /*call*/, BrainHttpClient& /*http*/, uint32 /*budgetMs*/) override
    {
        DecisionReply reply;
        reply.error = BrainError::NotBuilt;
        reply.detail = "openai-constrained is not built yet (M2b)";
        return reply;
    }

private:
    BrainEngineConfig const config;
};

#endif
