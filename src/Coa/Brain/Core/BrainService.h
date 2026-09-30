/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINSERVICE_H
#define PLAYERBOTS_BRAINSERVICE_H

#include "BrainConfig.h"
#include "IBrain.h"
#include <memory>
#include <string>

class InProcessBrain;

// Owns the running brain and swaps it in and out. The instance is a shared_ptr swapped under a small mutex:
// callers copy it and keep it for the call, so .playerbots brain off, a reload or a restart never frees a brain
// under a caller. With AiPlayerbot.CoaBrain.Enable = 0 nothing here starts a thread or opens a socket.
namespace BrainService
{
// Any thread. The running brain, or a NullBrain. Never null.
std::shared_ptr<IBrain> Current();

// Any thread, one relaxed load: running, accepting, CoaBrain.Use.ChatReply, and a usable engine in the chain.
// The director tests this before anything else, so with the brain off its path is exactly M1's.
bool ChatActive();

// Any thread. Whether a brain ever ran in this process (CoaWorldScript then pumps BrainUsageStore and BrainAdmin).
bool EverStarted();

// World thread. Makes the service match `config` and the runtime override: starts, stops, restarts (Workers,
// CaBundle or TlsVerify changed) or reconfigures the brain; configures BrainGovernor and BrainCache; publishes
// `config` (BrainConfig::Publish). The caller has loaded the usage (BrainUsageStore::LoadSync) and the question
// set (QuestionRegistry::Reload) before a start. Logs one INFO line when the brain starts or stops.
void Apply(std::shared_ptr<BrainConfig const> config);

// World thread (.playerbots brain on|off). -1: follow the conf; 0: off; 1: on. .reload config sets it back to -1.
void SetOverride(int32 state);
int32 Override();

// World thread. WORLDHOOK_ON_SHUTDOWN_INITIATE: submits stop. WORLDHOOK_ON_SHUTDOWN_CANCEL: they resume.
void ShutdownInitiate();
void ShutdownCancel();

// World thread, WORLDHOOK_ON_SHUTDOWN (also reached by World::StopNow, which never fires OnShutdownInitiate).
// InProcessBrain::Shutdown(Shutdown.WorkerDeadlineMs), then a NullBrain for good: Apply does nothing afterwards.
void Shutdown();

// World thread. The running brain (status, probe), null when off.
std::shared_ptr<InProcessBrain> Running();

// World thread. Why the last Apply left the brain off although it was asked on ("no usable engine: ...",
// "Boost.JSON missing"), "" otherwise.
std::string LastProblem();
}  // namespace BrainService

#endif
