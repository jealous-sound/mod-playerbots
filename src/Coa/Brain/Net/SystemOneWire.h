/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SYSTEMONEWIRE_H
#define PLAYERBOTS_SYSTEMONEWIRE_H

#include "BrainTypes.h"
#include "IDecisionEngine.h"
#include <cstddef>
#include <string>
#include <string_view>

struct BrainJsonValue;
struct SystemOneDialect;

// The SystemOne decision wire shared by openrouter, typesafe, jevlocal and the mock: builds request bodies and
// reads answers, with every field name taken from the dialect. Pure functions; any thread (workers, self-test).
namespace SystemOneWire
{
constexpr std::size_t MaxResponseBytes = 64 * 1024;
constexpr std::size_t MaxPlayerTextBytes = 255;

// The state as this engine receives it: job->state, then, only when call.sendPlayerText and the job has player
// text, the fenced [player text] section (see the contract), the whole cut to call.stateMaxChars bytes on a UTF-8
// boundary by shortening the player text first (dropped when fewer than 16 bytes would remain).
std::string StateAsSent(DecisionCall const& call);

// {"model", "state", "questions": {<name>: {"type", "instructions", "criteria"}}}. Choice criteria: an object of
// option key -> text, in option order. YesNo: "criteria" {"true", "false"} when the question has those two
// options, omitted otherwise. Score: an array of the level texts, lowest first.
std::string BuildRequest(SystemOneDialect const& dialect, std::string const& model, DecisionCall const& call);

// A 200 body. Fills reply.answers (one per job question, same order), reply.usage (cost = usage.cost in USD when
// present, else 0 and costReported false: the caller prices tokens) and reply.providerModel. False, with
// reply.error = BadResponse, when the body is not an object with an answers object, or when no answer is valid;
// an error object inside a 200 is classified like a status (ClassifyStatus).
bool ParseResponse(SystemOneDialect const& dialect, BrainJob const& job, std::string_view body,
                   bool engineCalibrated, float uncalibratedConfidence, DecisionReply& reply);

// One answer object against its question. Choice: `choice` must be an option key; probabilities keep only
// option keys, renormalized when they sum > 0; p = probabilities[choice], else confidence, else
// uncalibratedConfidence (calibrated false). YesNo: noul in [0, 1] (calibrated when the engine is). Score: score
// in [0, levels - 1]; probabilities keyed "0".."n-1"; level = argmax, else round(score); p = probabilities[level]
// or uncalibratedConfidence. Missing, wrongly typed or out of range: valid = false.
BrainAnswer ParseAnswer(SystemOneDialect const& dialect, BrainQuestion const& question, BrainJsonValue const* answer,
                        bool engineCalibrated, float uncalibratedConfidence);

// A non-200 status (or a 200 with an error object: pass status 200). Exhausted when the status is in
// exhaustedStatuses or the error text holds an exhaustedMarker; then AuthFailed by authStatuses; then Misconfigured
// when the text holds a misconfiguredMarker (not for 408 or >= 500: "Model x does not exist" is a wrong Model,
// not a bad request); then BadRequest, Misconfigured by their lists; ServerError for 408, 5xx, 524, 529 and anything
// else. `detail`: the error text (error.message, error as a string, or detail), at most 300 bytes, control
// characters removed.
BrainError ClassifyStatus(SystemOneDialect const& dialect, int32 status, std::string_view body, std::string& detail);
}  // namespace SystemOneWire

#endif
