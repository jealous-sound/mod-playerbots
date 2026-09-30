/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_INPROCESSBRAIN_H
#define PLAYERBOTS_INPROCESSBRAIN_H

#include "BrainConfig.h"
#include "IBrain.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class BrainResolver;

// The running brain: a three-lane queue, CoaBrain.Workers worker threads, a DNS resolver thread, and the engine
// chain built from one BrainConfig snapshot.
//
// Threads. Submit/Cancel/Active run on game threads and take short locks only. Workers alone do network I/O
// (through their own BrainHttpClient); they never touch a Player, a PlayerbotAI or the world, and hand results
// back through the job's BrainSink. Start, Reconfigure, SetAccepting and Shutdown run on the world thread.
//
// Lifetime. Everything the workers use (queue, engine set, clients, stop flag) sits in a Shared block owned by
// shared_ptr: each worker holds one, so a worker detached at shutdown keeps its state alive and exits on its own.
// Reconfigure swaps the engine set; a call in flight finishes on the set it started with.
//
// M6. A second family lives beside the decision engines: the text generator (ITextEngine, from CoaBrain.Text.*).
// Its jobs (BrainJobKind::Text) wait in their own BrainQueue (the same three lanes) and are served by
// CoaBrain.Text.Workers threads of their own, each with its own BrainHttpClient, so a slow model call never holds a
// decision worker. The governor, the sink, the resolver, the usage table and the breaker are shared: the text
// generator is the governor row "text".
class InProcessBrain final : public IBrain
{
public:
    explicit InProcessBrain(std::shared_ptr<BrainConfig const> config);
    ~InProcessBrain() override;  // Shutdown(0) when still running

    // World thread. Builds the engines (BrainEngines::Create; unusable ones are kept, skipped and shown by
    // status) and the text engine when Text.Provider is usable (TextEngines::Create), starts the resolver for the
    // hosted engines' host names (the text host included), then the decision workers and the text workers. False,
    // with `error`, when no engine of the chain and no text engine is usable, or JSON support is missing
    // (BrainJson::Available()).
    bool Start(std::string& error);

    // World thread. New chain, engines, timeouts and privacy from `config`; the workers, the TLS settings and the
    // queue stay (BrainService restarts the brain when Workers, CaBundle or TlsVerify change).
    void Reconfigure(std::shared_ptr<BrainConfig const> config);

    // World thread. WORLDHOOK_ON_SHUTDOWN_INITIATE stops submits (false), WORLDHOOK_ON_SHUTDOWN_CANCEL
    // resumes them (true).
    void SetAccepting(bool accepting);
    bool Accepting() const { return accepting.load(std::memory_order_relaxed); }

    // World thread, once (WORLDHOOK_ON_SHUTDOWN, .playerbots brain off, a restart). Stops submits, closes the
    // queue (waiting jobs are dropped, their reservations settled, nothing posted), interrupts every worker's
    // HTTP client, waits up to deadlineMs for the workers to leave, then detaches the ones still running; stops
    // the resolver (detached, never joined).
    void Shutdown(uint32 deadlineMs);

    // IBrain. Active(Text): the text engine exists; the other uses: the decision chain has a usable engine.
    bool Active(BrainUse use) const override;
    bool ChainCalibrated() const override;
    BrainTicket Submit(BrainJob&& job, std::shared_ptr<BrainSink> const& sink, BrainResult* inlineHit) override;
    void Cancel(uint64 jobId) override;

    // Any thread.
    std::shared_ptr<BrainConfig const> Config() const;
    uint32 QueueDepth(BrainLane lane) const;  // the decision queue
    uint32 WorkerCount() const { return uint32(workers.size()); }
    // M6. The text queue's depth, the text workers, and whether a text engine exists / may get players' words.
    uint32 TextQueueDepth(BrainLane lane) const;
    uint32 TextWorkerCount() const { return uint32(textWorkers.size()); }
    bool TextUsable() const { return textUsable.load(std::memory_order_relaxed); }
    bool TextWithWords() const { return textWithWords.load(std::memory_order_relaxed); }

private:
    InProcessBrain(InProcessBrain const&) = delete;
    InProcessBrain& operator=(InProcessBrain const&) = delete;

    struct Shared;  // defined in InProcessBrain.cpp: queue, engine set, clients, stop flag, exit counter

    // Worker `index`: takes jobs until the queue closes. Owns a reference to `shared` for its whole life, so a
    // worker detached at shutdown never uses freed state.
    static void Work(std::shared_ptr<Shared> shared, uint32 index);
    // M6. Text worker `index` (its slot in Shared::done and Shared::clients follows the decision workers'): takes
    // text jobs until the text queue closes. Per job: Governor::Acquire("text"), ITextEngine::Generate, Record,
    // the job's BrainTextFinisher (a refusal of it makes the result Rejected, the call stays a success for the
    // governor), Settle, exactly one Post. Same try/catch and deadline rules as Work.
    static void WorkText(std::shared_ptr<Shared> shared, uint32 index);

    // World thread. Builds the engines of `config` and swaps the new set in; `problems` lists the engines left out.
    // False when no engine of the chain could be built.
    bool InstallEngines(std::shared_ptr<BrainConfig const> const& config, std::string& problems);

    std::shared_ptr<Shared> shared;
    std::shared_ptr<BrainResolver> resolver;
    std::vector<std::thread> workers;
    std::vector<std::thread> textWorkers;  // M6: empty when Text.Provider is none or unusable
    std::atomic<bool> accepting{false};
    std::atomic<bool> running{false};
    std::atomic<bool> useChatReply{false};     // the engine set's CoaBrain.Use.ChatReply
    std::atomic<bool> chainCalibrated{false};  // an engine of the set's chain is calibrated
    std::atomic<bool> chainUsable{false};      // M6: the set's chain holds a usable engine
    std::atomic<bool> textUsable{false};       // M6: the set has a text engine and text workers run
    std::atomic<bool> textWithWords{false};    // M6: that engine's effective SendPlayerText
    std::atomic<uint32> tooLateSkips{0};       // Submit's TooLate refusals, to let one in 32 through
    bool stopped = false;  // world thread
};

#endif
