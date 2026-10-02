/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SYSTEMONEDIALECTS_H
#define PLAYERBOTS_SYSTEMONEDIALECTS_H

#include "BrainConfig.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Everything that differs between providers of the SystemOne ("Jev") decision wire, and every JSON field name
// the engines read or write. Engine logic never spells a field name: it reads it from here, so a provider change
// is a one-line fix in SystemOneDialects.cpp or a CoaBrain.DialectFile override, with no engine change.
// The compiled-in values follow the documentation as of 2026-09-30 (unverified until a recorded fixture exists).
struct SystemOneDialect
{
    std::string name;  // "openrouter", "typesafe", "jevlocal", "mock", "openai-constrained"
    BrainDialect kind = BrainDialect::Unknown;

    // Defaults of Engine.<n>.*.
    std::string defaultUrl;
    std::vector<std::string> alternateUrls;  // suggested by probe after an HTTP 404
    std::string defaultModel;
    bool hosted = true;        // a remote provider: https, key, no player text unless Privacy.SendPlayerText
    bool requiresKey = true;
    bool loopbackOnly = false; // jevlocal: its URL must be a loopback address
    bool calibratedByDefault = true;
    bool network = true;       // false for mock: no socket
    bool built = true;         // false for openai-constrained until M2b

    // Request.
    std::string authHeader = "Authorization";
    std::string authPrefix = "Bearer ";
    std::string fModel = "model";
    std::string fState = "state";
    std::string fQuestions = "questions";
    std::string fType = "type";
    std::string fInstructions = "instructions";
    std::string fCriteria = "criteria";
    std::string typeChoice = "choice";
    std::string typeYesNo = "noul";
    std::string typeScore = "score";
    std::string keyTrue = "true";  // YesNo criteria keys
    std::string keyFalse = "false";

    // Response.
    std::string fAnswers = "answers";
    std::string fChoice = "choice";
    std::string fYesNo = "noul";
    std::string fScore = "score";
    std::string fConfidence = "confidence";
    std::string fProbabilities = "probabilities";
    std::string fLegend = "legend";
    std::string fUsage = "usage";
    std::string fInputTokens = "input_tokens";
    std::string fOutputTokens = "output_tokens";
    std::string fCost = "cost";  // USD; OpenRouter only
    std::string fId = "id";
    std::string fResponseModel = "model";
    std::string fProvider = "provider";
    std::string fError = "error";         // an object (OpenRouter) or a string
    std::string fErrorMessage = "message";
    std::string fErrorCode = "code";
    std::string fDetail = "detail";       // FastAPI-style validation errors (TypeSafe 422, unverified)
    std::string fDetailMessage = "msg";   // the text of an entry when `detail` is an array

    // Error classes by HTTP status (anything else >= 500, 408, 524, 529: ServerError).
    std::vector<int32> exhaustedStatuses{402, 429};
    std::vector<int32> badRequestStatuses{400, 413, 422};
    std::vector<int32> authStatuses{401, 403};
    std::vector<int32> misconfiguredStatuses{404};
    // Lower-case substrings of the error text that mean the money is spent, whatever the status.
    std::vector<std::string> exhaustedMarkers{"spend limit", "insufficient credits", "insufficient_quota",
                                              "credit limit", "out of credits", "budget exceeded"};
    // Lower-case substrings of the error text that mean a setting is wrong (OpenRouter answers an unknown Model
    // with 400 "Model <name> does not exist"). Checked after the auth statuses (its 401 says "User not found.") and
    // never for a server error (408, 5xx), which stays transient.
    std::vector<std::string> misconfiguredMarkers{"does not exist", "not found"};
};

namespace SystemOneDialects
{
// World thread (BrainConfig::Load). The compiled-in table, then the overrides of `file` when not empty: a JSON
// object {"<dialect>": {"<member name above>": value, ...}, ...} (strings, booleans, arrays of numbers or
// strings). Unknown dialects or members are logged once and ignored; a broken file leaves the compiled-in table.
void Reload(std::string const& file);

// Any thread. Null for an unknown name. The pointer stays valid after a reload (engines keep theirs).
std::shared_ptr<SystemOneDialect const> Find(std::string_view name);

// The compiled-in table, whatever the file says (the self-test uses it).
std::shared_ptr<SystemOneDialect const> BuiltIn(std::string_view name);

std::vector<std::string> Names();
std::string Source();  // "built-in" or the file's path
}  // namespace SystemOneDialects

#endif
