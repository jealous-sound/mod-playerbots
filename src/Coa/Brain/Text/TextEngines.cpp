/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ITextEngine.h"
#include "BrainHttpClient.h"
#include "OpenAiChatEngine.h"
#include "AnthropicEngine.h"
#include "MockTextEngine.h"
#include <exception>

std::shared_ptr<ITextEngine> TextEngines::Create(BrainTextConfig const& config, BrainConfig const&,
                                               std::shared_ptr<BrainResolver> const& resolver, std::string& error)
{
    error.clear();
    try
    {
        if (!config.usable)
        {
            error = config.problem.empty() ? "not usable" : config.problem;
            return nullptr;
        }
        if (config.provider == BrainTextProvider::Mock)
            return std::make_shared<MockTextEngine>(config);
        BrainUrl url;
        if (!ParseBrainUrl(config.url, url, error))
        {
            error = "bad URL: " + error;
            return nullptr;
        }
        if (config.provider == BrainTextProvider::OpenAi)
            return std::make_shared<OpenAiChatEngine>(config, resolver, std::move(url));
        if (config.provider == BrainTextProvider::Anthropic)
            return std::make_shared<AnthropicEngine>(config, resolver, std::move(url));
        error = "unknown provider";
    }
    catch (std::exception const&)
    {
        error = "text engine creation failed";
    }
    catch (...)
    {
        error = "text engine creation failed";
    }
    return nullptr;
}
