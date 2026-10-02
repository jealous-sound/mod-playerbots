/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINRESOLVER_H
#define PLAYERBOTS_BRAINRESOLVER_H

#include "Define.h"
#include <memory>
#include <string>
#include <vector>

// Resolves the hosted engines' host names on its own thread, so neither a worker nor a game thread ever waits on
// getaddrinfo. It resolves every host at start, every DnsRefreshSeconds, and when a worker asks after a failed
// connect (at most once per 5 s per host). A failed lookup keeps the last good addresses.
//
// Lifetime. The thread holds the shared state through a shared_ptr and is detached, never joined: getaddrinfo
// cannot be interrupted. Stop only raises a flag; the thread leaves at its next wake-up and writes nothing
// (no log) once stopped.
class BrainResolver
{
public:
    // World thread. Starts the thread for these host names (IP literals are skipped: they need no lookup).
    static std::shared_ptr<BrainResolver> Start(std::vector<std::string> hosts, uint32 refreshSeconds);

    ~BrainResolver();  // Stop()

    // Any thread (workers). The cached addresses of `host` as IP literals, IPv4 first; empty when never resolved.
    std::vector<std::string> Lookup(std::string const& host) const;

    // Any thread (a worker after a connect failure). Asks for a new lookup of `host` soon.
    void RequestRefresh(std::string const& host);

    // World thread (InProcessBrain::Reconfigure). Hosts added are resolved at once; removed ones are forgotten.
    void SetHosts(std::vector<std::string> hosts, uint32 refreshSeconds);

    // World thread. Idempotent.
    void Stop();

    // Any thread (status): hosts known, hosts with at least one address.
    uint32 HostCount() const;
    uint32 ResolvedCount() const;

private:
    BrainResolver() = default;
    BrainResolver(BrainResolver const&) = delete;
    BrainResolver& operator=(BrainResolver const&) = delete;

    struct State;  // mutex, condition variable, host table, stop flag: shared with the thread
    std::shared_ptr<State> state;
};

#endif
