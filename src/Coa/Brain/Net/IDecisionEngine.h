/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_IDECISIONENGINE_H
#define PLAYERBOTS_IDECISIONENGINE_H

#include "BrainConfig.h"
#include "BrainTypes.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class BrainHttpClient;
class BrainResolver;

// One call to one engine, as the worker hands it over. Everything is borrowed for the duration of Decide.
struct DecisionCall
{
    BrainJob const* job = nullptr;  // state, player text, questions
    bool sendPlayerText = false;    // this engine may receive job->playerText (BrainEngineConfig::sendPlayerText)
    uint32 stateMaxChars = 3000;    // the state as sent, player text included, is cut to this
    uint32 connectTimeoutMs = 2000;
    bool record = false;                        // keep the raw bodies in the reply (probe record)
    std::atomic<bool> const* cancel = nullptr;  // raised at shutdown: stop waiting, return Cancelled
};

struct DecisionReply
{
    BrainError error = BrainError::None;
    int32 httpStatus = 0;
    std::vector<BrainAnswer> answers;  // one per job question, same order; unparsable ones have valid = false
    BrainUsage usage;
    bool calibrated = false;  // the engine's Calibrated flag
    uint32 ms = 0;
    std::string providerModel;
    std::string detail;       // error excerpt, at most 300 bytes, never a key or a header
    std::string url;          // record only
    std::string rawRequest;   // record only
    std::string rawResponse;  // record only
};

// A decision engine: turns one job into typed answers. Engines are built on the world thread and then shared by
// every worker, so Decide must be thread-safe: an engine holds only immutable configuration, and per-call state
// lives on the stack and in `http`, the calling worker's own client. Never touches a game object; never logs a key.
class IDecisionEngine
{
public:
    virtual ~IDecisionEngine() = default;

    virtual BrainEngineConfig const& Config() const = 0;
    virtual bool Calibrated() const = 0;

    // Worker thread. At most budgetMs of wall time (connect, TLS, write, read); honours call.cancel.
    virtual DecisionReply Decide(DecisionCall const& call, BrainHttpClient& http, uint32 budgetMs) = 0;
};

namespace BrainEngines
{
// World thread (InProcessBrain::Start/Reconfigure). The engine for `config`: JevEngine for openrouter, typesafe
// and jevlocal; MockDecisionEngine for mock (fixtures from brain.fixtureDir); the OpenAiConstrained stub. Null,
// with `error`, when the config is not usable (config.usable false, unknown dialect, bad URL). A mock whose
// fixtures directory is missing is still built: it answers at random.
std::shared_ptr<IDecisionEngine> Create(BrainEngineConfig const& config, BrainConfig const& brain,
                                        std::shared_ptr<BrainResolver> const& resolver, std::string& error);
}  // namespace BrainEngines

#endif
