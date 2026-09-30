/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainResolver.h"
#include "CoaTime.h"
#include "Log.h"
#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <condition_variable>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
constexpr uint64 FailureRetryMs = 30000;
constexpr uint64 MinAskIntervalMs = 5000;
constexpr uint32 MinRefreshSeconds = 30;

bool IsIpLiteral(std::string const& host)
{
    boost::system::error_code ec;
    boost::asio::ip::make_address(host, ec);
    return !ec;
}

std::vector<std::string> UniqueHosts(std::vector<std::string> hosts)
{
    std::vector<std::string> out;
    for (std::string& host : hosts)
    {
        if (host.empty() || IsIpLiteral(host) || std::find(out.begin(), out.end(), host) != out.end())
            continue;
        out.push_back(std::move(host));
    }
    return out;
}

uint64 RefreshMs(uint32 refreshSeconds) { return uint64(std::max(refreshSeconds, MinRefreshSeconds)) * 1000; }
}  // namespace

struct BrainResolver::State
{
    struct Host
    {
        std::vector<std::string> addresses;  // IPv4 first
        uint64 nextMs = 0;                   // due for a lookup at this time (0: now)
        uint64 lastAskMs = 0;
        bool asked = false;
        bool warned = false;  // one WARN per host until it resolves again
    };

    std::mutex lock;
    std::condition_variable wake;
    bool stop = false;
    uint64 refreshMs = 300000;
    std::map<std::string, Host> hosts;

    // getaddrinfo, outside the lock. Addresses as literals, IPv4 first, without duplicates.
    static bool Resolve(boost::asio::ip::tcp::resolver& resolver, std::string const& host,
                        std::vector<std::string>& addresses, std::string& error)
    {
        addresses.clear();
        try
        {
            boost::system::error_code ec;
            auto const results = resolver.resolve(host, "443", ec);
            if (ec)
            {
                error = ec.message();
                return false;
            }

            std::vector<std::string> v4;
            std::vector<std::string> v6;
            for (auto const& entry : results)
            {
                boost::asio::ip::address const address = entry.endpoint().address();
                std::vector<std::string>& list = address.is_v4() ? v4 : v6;
                std::string text = address.to_string();
                if (std::find(list.begin(), list.end(), text) == list.end())
                    list.push_back(std::move(text));
            }
            addresses = std::move(v4);
            addresses.insert(addresses.end(), v6.begin(), v6.end());
            if (addresses.empty())
            {
                error = "no address";
                return false;
            }
            return true;
        }
        catch (std::exception const& e)
        {
            error = e.what();
        }
        catch (...)
        {
            error = "exception";
        }
        return false;
    }

    // The thread's body. It owns `state` through the shared_ptr, so it may outlive the BrainResolver (detached,
    // never joined: getaddrinfo cannot be interrupted). Once stopped it writes nothing, not even a log line.
    static void Run(std::shared_ptr<State> state)
    {
        try
        {
            boost::asio::io_context ioc;
            boost::asio::ip::tcp::resolver resolver(ioc);

            std::unique_lock<std::mutex> guard(state->lock);
            while (!state->stop)
            {
                uint64 const now = CoaTime::NowMs();
                uint64 earliest = std::numeric_limits<uint64>::max();
                std::vector<std::string> due;
                for (auto& [name, host] : state->hosts)
                {
                    if (host.nextMs <= now)
                    {
                        due.push_back(name);
                        // Provisional: a refresh asked during the lookup does not repeat it at once.
                        host.nextMs = now + std::min(FailureRetryMs, state->refreshMs);
                    }
                    else
                        earliest = std::min(earliest, host.nextMs);
                }

                if (due.empty())
                {
                    if (earliest == std::numeric_limits<uint64>::max())
                        state->wake.wait(guard);
                    else
                        state->wake.wait_for(guard, std::chrono::milliseconds(earliest - now));
                    continue;
                }

                guard.unlock();
                for (std::string const& name : due)
                {
                    std::vector<std::string> addresses;
                    std::string error;
                    bool const ok = Resolve(resolver, name, addresses, error);

                    guard.lock();
                    if (state->stop)
                        return;

                    bool warn = false;
                    auto it = state->hosts.find(name);
                    if (it != state->hosts.end())
                    {
                        uint64 const after = CoaTime::NowMs();
                        if (ok)
                        {
                            it->second.addresses = std::move(addresses);
                            it->second.nextMs = after + state->refreshMs;
                            it->second.warned = false;
                        }
                        else
                        {
                            it->second.nextMs = after + std::min(FailureRetryMs, state->refreshMs);
                            warn = !it->second.warned;
                            it->second.warned = true;
                        }
                    }
                    guard.unlock();

                    if (warn)
                        LOG_WARN("playerbots.brain",
                                 "CoA brain: cannot resolve {} ({}); its engine answers Dns until it resolves", name,
                                 error);
                }
                guard.lock();
            }
        }
        catch (...)
        {
            // Nothing to report to: a failed resolver leaves every lookup empty, and the engines answer Dns.
        }
    }
};

std::shared_ptr<BrainResolver> BrainResolver::Start(std::vector<std::string> hosts, uint32 refreshSeconds)
{
    std::shared_ptr<BrainResolver> resolver(new BrainResolver());
    resolver->state = std::make_shared<State>();
    resolver->state->refreshMs = RefreshMs(refreshSeconds);
    for (std::string& host : UniqueHosts(std::move(hosts)))
        resolver->state->hosts.emplace(std::move(host), State::Host());

    try
    {
        std::thread(&State::Run, resolver->state).detach();
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("playerbots.brain", "CoA brain: the DNS thread could not start ({}); hosted engines answer Dns",
                  e.what());
    }
    return resolver;
}

BrainResolver::~BrainResolver() { Stop(); }

std::vector<std::string> BrainResolver::Lookup(std::string const& host) const
{
    if (!state)
        return {};

    std::lock_guard<std::mutex> guard(state->lock);
    auto it = state->hosts.find(host);
    return it != state->hosts.end() ? it->second.addresses : std::vector<std::string>();
}

void BrainResolver::RequestRefresh(std::string const& host)
{
    if (!state)
        return;

    std::lock_guard<std::mutex> guard(state->lock);
    auto it = state->hosts.find(host);
    if (it == state->hosts.end())
        return;

    uint64 const now = CoaTime::NowMs();
    if (it->second.asked && now - it->second.lastAskMs < MinAskIntervalMs)
        return;

    it->second.asked = true;
    it->second.lastAskMs = now;
    it->second.nextMs = now;
    state->wake.notify_one();
}

void BrainResolver::SetHosts(std::vector<std::string> hosts, uint32 refreshSeconds)
{
    if (!state)
        return;

    std::vector<std::string> const wanted = UniqueHosts(std::move(hosts));

    std::lock_guard<std::mutex> guard(state->lock);
    uint64 const refreshMs = RefreshMs(refreshSeconds);
    if (refreshMs < state->refreshMs)
    {
        // A shorter period applies now, not after the current one runs out.
        uint64 const now = CoaTime::NowMs();
        for (auto& [name, host] : state->hosts)
            host.nextMs = std::min(host.nextMs, now + refreshMs);
    }
    state->refreshMs = refreshMs;

    for (auto it = state->hosts.begin(); it != state->hosts.end();)
    {
        if (std::find(wanted.begin(), wanted.end(), it->first) == wanted.end())
            it = state->hosts.erase(it);
        else
            ++it;
    }
    for (std::string const& host : wanted)
        state->hosts.emplace(host, State::Host());

    state->wake.notify_one();
}

void BrainResolver::Stop()
{
    if (!state)
        return;

    std::lock_guard<std::mutex> guard(state->lock);
    state->stop = true;
    state->wake.notify_all();
}

uint32 BrainResolver::HostCount() const
{
    if (!state)
        return 0;

    std::lock_guard<std::mutex> guard(state->lock);
    return uint32(state->hosts.size());
}

uint32 BrainResolver::ResolvedCount() const
{
    if (!state)
        return 0;

    std::lock_guard<std::mutex> guard(state->lock);
    uint32 count = 0;
    for (auto const& [name, host] : state->hosts)
        if (!host.addresses.empty())
            ++count;
    return count;
}
