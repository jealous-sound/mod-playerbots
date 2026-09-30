/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_JEVENGINE_H
#define PLAYERBOTS_JEVENGINE_H

#include "BrainHttpClient.h"
#include "IDecisionEngine.h"
#include <memory>
#include <string>

struct SystemOneDialect;

// TypeSafe's Jev behind the SystemOne wire, for the openrouter, typesafe and jevlocal dialects. Decide: the
// address from the resolver (or the URL's IP literal; none -> Dns and a refresh request), the body from
// SystemOneWire::BuildRequest, one POST through the worker's client, then ParseResponse or ClassifyStatus. On
// Network or Timeout it asks the resolver to refresh the host. Usage cost: usage.cost when reported, else
// input x PriceInPerMTok + output x PriceOutPerMTok, else the request bytes / 3.5 x PriceInPerMTok.
class JevEngine final : public IDecisionEngine
{
public:
    JevEngine(BrainEngineConfig config, std::shared_ptr<SystemOneDialect const> dialect,
              std::shared_ptr<BrainResolver> resolver, BrainUrl url);

    BrainEngineConfig const& Config() const override { return config; }
    bool Calibrated() const override { return config.calibrated; }
    DecisionReply Decide(DecisionCall const& call, BrainHttpClient& http, uint32 budgetMs) override;

private:
    BrainEngineConfig const config;
    std::shared_ptr<SystemOneDialect const> const dialect;
    std::shared_ptr<BrainResolver> const resolver;  // null for loopback and IP-literal URLs
    BrainUrl const url;
    std::string const authorization;  // dialect->authPrefix + key, or empty; never logged
};

#endif
