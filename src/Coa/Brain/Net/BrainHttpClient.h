/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BRAINHTTPCLIENT_H
#define PLAYERBOTS_BRAINHTTPCLIENT_H

#include "BrainTypes.h"
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// An http:// or https:// URL, split. Pure; any thread.
struct BrainUrl
{
    bool tls = true;
    std::string host;  // IPv6 literals without their brackets
    uint16 port = 443;
    std::string target = "/";  // path and query
    bool hostIsIp = false;
    bool loopback = false;  // 127.0.0.0/8, ::1 or "localhost"
};

// False, with `error`, for anything but http(s)://host[:port][/path]: no user info, no fragment, port 1-65535.
bool ParseBrainUrl(std::string_view text, BrainUrl& out, std::string& error);

struct BrainTlsSettings
{
    bool verify = true;    // CoaBrain.TlsVerify: peer certificate and host name
    std::string caBundle;  // CoaBrain.CaBundle: a PEM file; empty = the system's roots
};

struct BrainHttpRequest
{
    BrainUrl const* url = nullptr;  // Host header, SNI and certificate name come from url->host
    std::string address;            // the IP literal to connect to (from BrainResolver, or the URL's own)
    std::string authHeader = "Authorization";
    std::string authorization;  // the header's whole value ("Bearer ..."); empty: no header. Never logged.
    std::string body;           // JSON, sent as application/json
    // M6: further headers (Anthropic's anthropic-version and anthropic-beta), sent after the auth header. Names are
    // tokens and values have no CR or LF (Post refuses the request with BadResponse otherwise); never logged.
    std::vector<std::pair<std::string, std::string>> extraHeaders;
    uint32 connectTimeoutMs = 2000;
    uint32 totalTimeoutMs = 4000;  // connect, TLS handshake, write and read together
};

struct BrainHttpResponse
{
    BrainError transport = BrainError::None;  // None: a status line arrived; else Network, Timeout, Cancelled,
                                              // BadResponse (over 64 KiB, broken HTTP)
    int32 status = 0;
    std::string body;
    std::string detail;  // transport error text (no header, no body)
    uint32 ms = 0;
    bool reused = false;  // sent on a kept-alive connection
    bool sent = false;    // the request may have reached the server (writing began): never resend it elsewhere
};

// A blocking HTTP/1.1 client for one worker thread: Boost.Beast over its own io_context, TLS through
// boost::asio::ssl on the core's OpenSSL. Every Beast and Asio type stays in BrainHttpClient.cpp.
//
// Not thread-safe except Interrupt: each worker owns one. It never resolves a name: it connects to
// request.address and sets Host, SNI and the certificate check to url->host. It keeps at most one idle connection
// per (host, port, tls) and reuses it; a reused connection that fails before any response byte is retried once on
// a fresh one when time is left. Interim 1xx responses are skipped (at most 8); a connection is kept only after a
// clean final response (keep-alive, no 1xx before it, nothing left unread). Responses over 64 KiB or headers over
// 8 KiB are refused. No exception escapes.
class BrainHttpClient
{
public:
    explicit BrainHttpClient(BrainTlsSettings const& tls);
    ~BrainHttpClient();

    // Whether the TLS context is ready (roots loaded). When false, https requests fail with Network and
    // TlsProblem() says why; http requests still work.
    bool TlsReady() const;
    std::string const& TlsProblem() const;

    // The owning worker only.
    BrainHttpResponse Post(BrainHttpRequest const& request);

    // Any thread (InProcessBrain::Shutdown). A Post in progress returns Cancelled promptly; later ones at once.
    void Interrupt();

    // The owning worker only: drops kept-alive connections idle for longer than idleMs.
    void CloseIdle(uint32 idleMs);

private:
    BrainHttpClient(BrainHttpClient const&) = delete;
    BrainHttpClient& operator=(BrainHttpClient const&) = delete;

    struct Impl;  // io_context, ssl::context, idle connections
    std::unique_ptr<Impl> impl;
};

#endif
