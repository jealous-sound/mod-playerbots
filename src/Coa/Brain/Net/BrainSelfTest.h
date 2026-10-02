/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINSELFTEST_H
#define PLAYERBOTS_BRAINSELFTEST_H

#include "Define.h"
#include <string>
#include <vector>

// Offline check of the wire code against the fixtures (.playerbots brain test selftest): no network, no game
// state, no brain needed. For each fixture: rebuild the job from its request, parse its response (or classify
// its error status) through SystemOneWire with the compiled-in dialect, compare with its "expect" block, then
// build the request again and compare it with the fixture's request (same state, question names, types,
// instructions and criteria). Any thread; the command runs it on the world thread (a few ms).
namespace BrainSelfTest
{
struct Report
{
    uint32 passed = 0;
    uint32 failed = 0;
    std::vector<std::string> lines;  // "PASS openrouter-voice-v1", "FAIL typesafe-voice-v1: line: choice ..."
};

// `dir`: CoaBrain.FixtureDir (every *.json in it). Empty, or a directory without fixtures: the compiled-in
// fixtures of BrainSelfTest.cpp (copies of the documented ones).
Report Run(std::string const& dir);
}  // namespace BrainSelfTest

#endif
