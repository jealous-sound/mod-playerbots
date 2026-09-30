/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MOCKDECISIONENGINE_H
#define PLAYERBOTS_MOCKDECISIONENGINE_H

#include "IDecisionEngine.h"
#include <memory>
#include <string>
#include <vector>

struct SystemOneDialect;

// An engine without network, for tests and soaks (dialect "mock"). Each call waits Engine.<n>.LatencyMs (in 20 ms
// slices, honouring call.cancel and the budget: past the budget -> Timeout), then fails ErrorPercent of the time
// with FailStatus (a 500 when 0) and that fixture's error body, or answers. An answer is a response body in the
// SystemOne shape, built from the fixture whose questions best match the job's (same names and types), with each
// choice kept when it is one of the job's option keys and drawn at random among them otherwise, then read back
// through SystemOneWire::ParseResponse, so the mock always exercises the real parser. No fixture: random valid
// answers. usage.cost is UsdPerCall. Fixtures are read once, at construction (world thread), from the fixture
// directory (*.json in the format of data/coa/brain/README.md); a 200 fixture whose expect.error is present and not
// "none" documents a failure and is left to the self-test: it never becomes a 200 reply.
class MockDecisionEngine final : public IDecisionEngine
{
public:
    MockDecisionEngine(BrainEngineConfig config, std::shared_ptr<SystemOneDialect const> dialect,
                       std::string const& fixtureDir);

    BrainEngineConfig const& Config() const override { return config; }
    bool Calibrated() const override { return config.calibrated; }
    DecisionReply Decide(DecisionCall const& call, BrainHttpClient& http, uint32 budgetMs) override;

    uint32 FixtureCount() const { return uint32(fixtures.size()); }

private:
    struct Fixture
    {
        std::string name;
        int32 status = 200;
        std::string responseBody;                // serialized "response" of the fixture file
        std::vector<std::string> questionNames;  // of its request, for matching
        std::vector<std::string> questionTypes;  // wire type names, same order
    };

    BrainEngineConfig const config;
    std::shared_ptr<SystemOneDialect const> const dialect;
    std::vector<Fixture> fixtures;  // immutable after construction
};

#endif
