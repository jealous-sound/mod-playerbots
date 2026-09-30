/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINADMIN_H
#define PLAYERBOTS_BRAINADMIN_H

#include "BrainTypes.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Prints one line to whoever asked: a player's session found by account id, or the console (account 0). Given by
// CoaWorldScript, which knows sessions; BrainAdmin knows none.
using BrainAdminPrint = std::function<void(uint32 requesterAccount, std::string const& line)>;

// One probe case turned into a job by BrainStateBuilder::BuildProbeJobs.
struct BrainProbeJob
{
    BrainJob job;  // use Probe, lane Organizer; BrainAdmin sets forceEngine, record and the deadline
    std::string caseId;
    std::string locale;  // "fr", "en"
    bool expectReply = true;
    std::string expectGroup;  // expected reply name, "" for none
    std::vector<std::pair<std::string, std::string>> lineGroups;  // line option key -> reply name
};

// A .playerbots brain test job, built by BrainStateBuilder::BuildTestJob.
struct BrainTestJob
{
    BrainJob job;  // use Test, lane Player, chat_reply questions; BrainAdmin sets the deadline
    std::string botName;
    std::string scope;  // CoaVoiceScopeName of the simulated message ("whisper")
    float threshold = 0.35f;  // CoaVoice.Threshold.<scope> the director would apply
    std::vector<std::pair<std::string, std::string>> lines;  // line option key -> the row, filled for the bot
};

// Backends of .playerbots brain status|on|off|reset|explain|test|probe. World thread only (chat and console
// commands run there). Test and probe answer later: their jobs post to BrainAdmin's own sink, and Update prints
// the report through the printer when every result is in (or after 60 s).
namespace BrainAdmin
{
// The status lines (see the contract): state, budget, one line per engine, counters, cache.
std::vector<std::string> Status();

// Loads the usage and the question set if needed, then BrainService::SetOverride(1) and
// Apply(BrainConfig::Load(true)). Lines say what happened (chain, or why it did not start).
std::vector<std::string> TurnOn();

// BrainService::SetOverride(0) and Apply. Decisions in flight fall back to the heuristic at their deadline.
std::vector<std::string> TurnOff();

// BrainGovernor::ResetFlags and BrainCache::Clear.
std::vector<std::string> Reset();

// The bot's last 10 entries of the decision ring.
std::vector<std::string> Explain(uint64 botGuid, std::string const& botName);

// Submits the test job (no cache, the chain); the answer is printed to `requester` when it arrives.
std::vector<std::string> StartTest(uint32 requesterAccount, BrainTestJob&& test);

// Submits every case to one engine ("" = the first usable of the chain), bypassing cache and chain, within
// Probe.MaxPerDay and the budgets. `record` also writes each call as a fixture file under Probe.RecordDir
// (refused when it is empty). One probe at a time: a second is refused while the first runs.
std::vector<std::string> StartProbe(uint32 requesterAccount, std::string const& engine, bool record,
                                    std::vector<BrainProbeJob>&& jobs);

// .playerbots brain test selftest: BrainSelfTest::Run(CoaBrain.FixtureDir), offline.
std::vector<std::string> SelfTest();

// World thread, every CoaWorldScript::OnUpdate once the brain ever ran: drains the admin sink, prints finished
// tests and probes, writes recorded fixtures, drops requests older than 60 s with a timeout line.
void Update(uint64 nowMs, BrainAdminPrint const& print);

// World thread, WORLDHOOK_ON_SHUTDOWN: closes the admin sink, forgets pending requests.
void Shutdown();
}  // namespace BrainAdmin

#endif
