/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "IDecisionEngine.h"
#include "BrainHttpClient.h"
#include "JevEngine.h"
#include "MockDecisionEngine.h"
#include "OpenAiConstrainedDecisionEngine.h"
#include "SystemOneDialects.h"
#include <exception>

std::shared_ptr<IDecisionEngine> BrainEngines::Create(BrainEngineConfig const& config, BrainConfig const& brain,
                                                      std::shared_ptr<BrainResolver> const& resolver,
                                                      std::string& error)
{
    error.clear();
    try
    {
        if (!config.usable)
        {
            error = config.problem.empty() ? "not usable" : config.problem;
            return nullptr;
        }

        std::shared_ptr<SystemOneDialect const> dialect = SystemOneDialects::Find(config.dialectName);
        if (!dialect)
        {
            error = "dialect table has no " + config.dialectName;
            return nullptr;
        }

        switch (config.dialect)
        {
            case BrainDialect::OpenRouter:
            case BrainDialect::TypeSafe:
            case BrainDialect::JevLocal:
            {
                BrainUrl url;
                std::string urlError;
                if (!ParseBrainUrl(config.url, url, urlError))
                {
                    error = "bad URL: " + urlError;
                    return nullptr;
                }
                // IP literals and loopback names need no lookup ("localhost" is mapped to 127.0.0.1 by the engine).
                bool const direct = url.hostIsIp || url.loopback;
                return std::make_shared<JevEngine>(config, std::move(dialect), direct ? nullptr : resolver,
                                                   std::move(url));
            }
            case BrainDialect::Mock:
                return std::make_shared<MockDecisionEngine>(config, std::move(dialect),
                                                            BrainConfig::ResolvePath(brain.fixtureDir));
            case BrainDialect::OpenAiConstrained:
                return std::make_shared<OpenAiConstrainedDecisionEngine>(config);
            default:
                error = "unknown dialect '" + config.dialectName + "'";
                return nullptr;
        }
    }
    catch (std::exception const& e)
    {
        error = std::string("engine creation failed: ") + e.what();
    }
    catch (...)
    {
        error = "engine creation failed";
    }
    return nullptr;
}
