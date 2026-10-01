/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BrainHttpClient.h"
#include "BrainCertStore.h"
#include "CoaText.h"
#include "CoaTime.h"
#include <algorithm>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cctype>
#include <chrono>
#include <exception>
#include <map>
#include <openssl/ssl.h>
#include <optional>
#include <utility>

namespace
{
namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace http = boost::beast::http;

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using TlsStream = ssl::stream<beast::tcp_stream>;

constexpr std::size_t MaxBodyBytes = 64 * 1024;
constexpr uint32 MaxHeaderBytes = 8 * 1024;
constexpr auto RetryMinLeft = Ms(300);
constexpr auto HardStopSlack = Ms(100);
constexpr std::size_t MaxDetailBytes = 300;
constexpr uint32 MaxInterimResponses = 8;
char const UserAgent[] = "mod-playerbots-coa-brain/1";

std::string Lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Excerpt(std::string text)
{
    CoaText::CutUtf8Bytes(text, MaxDetailBytes);
    return text;
}

// One kept-alive (or being opened) connection: exactly one of the two streams is set.
struct Connection
{
    std::unique_ptr<TlsStream> tls;
    std::unique_ptr<beast::tcp_stream> plain;
    uint64 lastUsedMs = 0;

    beast::tcp_stream& Lowest() { return tls ? beast::get_lowest_layer(*tls) : *plain; }
};

struct StepResult
{
    BrainError error = BrainError::None;
    std::string detail;
};
}  // namespace

struct BrainHttpClient::Impl
{
    // Declared first, destroyed last: the streams below are closed before their io_context goes.
    asio::io_context ioc{1};
    std::unique_ptr<ssl::context> ctx;
    bool verify = true;
    bool tlsReady = false;
    std::string tlsProblem;
    std::atomic<bool> interrupted{false};
    std::map<std::string, Connection> idle;  // "host:port:tls|tcp" -> at most one idle connection

    // Runs the io_context until the operation started just before sets `done`. Past `hardStop`, or when Interrupt
    // stops the context, it closes the socket so the operation completes (aborted) and keeps running until it has:
    // no handler may outlive the stack variables it points to. False when it had to abort.
    bool Drive(bool const& done, Clock::time_point hardStop, beast::tcp_stream& lowest)
    {
        // restart() then the flag: an Interrupt that raced with restart() is seen here, a later one stops run.
        ioc.restart();
        if (!interrupted.load())
        {
            while (!done)
            {
                std::size_t ran = 1;
                try
                {
                    ran = ioc.run_one_until(hardStop);
                }
                catch (...)
                {
                }
                if (!ran)
                    break;
            }
        }

        if (done)
        {
            // Beast's cancelled timer waits are ready now: run them so they do not linger.
            try
            {
                ioc.poll();
            }
            catch (...)
            {
            }
            return true;
        }

        boost::system::error_code ignored;
        lowest.socket().close(ignored);
        while (!done)
        {
            ioc.restart();
            try
            {
                ioc.run_one_for(Ms(100));
            }
            catch (...)
            {
            }
        }
        return false;
    }

    StepResult Aborted(char const* phase) const
    {
        StepResult r;
        if (interrupted.load())
        {
            r.error = BrainError::Cancelled;
            r.detail = "interrupted (shutdown)";
        }
        else
        {
            r.error = BrainError::Timeout;
            r.detail = std::string("timed out while ") + phase;
        }
        return r;
    }

    StepResult Map(boost::system::error_code const& ec, char const* phase) const
    {
        StepResult r;
        if (ec == beast::error::timeout)
        {
            r.error = BrainError::Timeout;
            r.detail = std::string("timed out while ") + phase;
        }
        else if (ec == http::error::body_limit || ec == http::error::buffer_overflow)
        {
            r.error = BrainError::BadResponse;
            r.detail = "response over 64 KiB";
        }
        else if (ec == http::error::header_limit)
        {
            r.error = BrainError::BadResponse;
            r.detail = "response headers over 8 KiB";
        }
        else if (ec == http::error::end_of_stream || ec == http::error::partial_message)
        {
            r.error = BrainError::Network;
            r.detail = std::string("connection closed while ") + phase;
        }
        else if (ec.category() == http::make_error_code(http::error::bad_version).category())
        {
            r.error = BrainError::BadResponse;
            r.detail = Excerpt("broken HTTP response: " + ec.message());
        }
        else if (ec == asio::error::operation_aborted && interrupted.load())
        {
            r.error = BrainError::Cancelled;
            r.detail = "interrupted (shutdown)";
        }
        else
        {
            r.error = BrainError::Network;
            r.detail = Excerpt(std::string(phase) + ": " + ec.message());
        }
        return r;
    }

    StepResult Open(Connection& conn, BrainUrl const& url, asio::ip::tcp::endpoint const& endpoint,
                    uint32 connectTimeoutMs, Clock::time_point deadline)
    {
        boost::system::error_code ec;
        if (url.tls)
        {
            conn.tls = std::make_unique<TlsStream>(ioc, *ctx);
            // SNI carries names only (RFC 6066); an IP literal is still checked against the certificate.
            if (!url.hostIsIp && !SSL_set_tlsext_host_name(conn.tls->native_handle(), url.host.c_str()))
                return {BrainError::Network, "cannot set the TLS server name"};
            if (verify)
            {
                conn.tls->set_verify_mode(ssl::verify_peer, ec);
                if (!ec)
                    conn.tls->set_verify_callback(ssl::host_name_verification(url.host), ec);
                if (ec)
                    return {BrainError::Network, Excerpt("TLS setup: " + ec.message())};
            }
        }
        else
            conn.plain = std::make_unique<beast::tcp_stream>(ioc);

        beast::tcp_stream& lowest = conn.Lowest();
        Clock::time_point const now = Clock::now();
        if (now >= deadline)
            return {BrainError::Timeout, "no time left to connect"};

        // The connect budget covers the TCP connect and the TLS handshake together.
        auto const connectBudget = std::min<Clock::duration>(Ms(connectTimeoutMs), deadline - now);
        lowest.expires_after(connectBudget);

        bool done = false;
        boost::system::error_code opEc;
        lowest.async_connect(endpoint,
                             [&done, &opEc](boost::system::error_code const& e)
                             {
                                 opEc = e;
                                 done = true;
                             });
        if (!Drive(done, deadline + HardStopSlack, lowest))
            return Aborted("connecting");
        if (opEc)
            return Map(opEc, "connecting");

        if (url.tls)
        {
            done = false;
            conn.tls->async_handshake(ssl::stream_base::client,
                                      [&done, &opEc](boost::system::error_code const& e)
                                      {
                                          opEc = e;
                                          done = true;
                                      });
            if (!Drive(done, deadline + HardStopSlack, lowest))
                return Aborted("in the TLS handshake");
            if (opEc)
                return Map(opEc, "in the TLS handshake");
        }
        return {};
    }

    struct Exchanged
    {
        StepResult step;
        bool sent = false;     // writing began: the server may have the request
        bool gotSome = false;  // at least one byte of the response arrived
        int32 status = 0;
        std::string body;
        bool keepAlive = false;  // the connection may be reused
    };

    template <class Stream>
    void Exchange(Stream& stream, beast::tcp_stream& lowest, http::request<http::string_body> const& message,
                  Clock::time_point deadline, Exchanged& out)
    {
        Clock::time_point const now = Clock::now();
        if (now >= deadline)
        {
            out.step = {BrainError::Timeout, "no time left to send the request"};
            return;
        }
        lowest.expires_after(deadline - now);

        bool done = false;
        boost::system::error_code opEc;
        out.sent = true;
        http::async_write(stream, message,
                          [&done, &opEc](boost::system::error_code const& e, std::size_t)
                          {
                              opEc = e;
                              done = true;
                          });
        if (!Drive(done, deadline + HardStopSlack, lowest))
        {
            out.step = Aborted("sending the request");
            return;
        }
        if (opEc)
        {
            out.step = Map(opEc, "sending the request");
            return;
        }

        // Beast ends a read at a 1xx header. Such an interim response (RFC 9110 15.2) is skipped with a fresh parser
        // on the same buffer until the final one; none is expected (no Expect: 100-continue), so the connection is
        // not kept after one. The stream expiry set above bounds the whole loop.
        beast::flat_buffer buffer;
        std::optional<http::response_parser<http::string_body>> parser;
        uint32 interims = 0;
        for (;;)
        {
            parser.emplace();
            parser->header_limit(MaxHeaderBytes);
            parser->body_limit(std::uint64_t(MaxBodyBytes));

            done = false;
            http::async_read(stream, buffer, *parser,
                             [&done, &opEc](boost::system::error_code const& e, std::size_t)
                             {
                                 opEc = e;
                                 done = true;
                             });
            bool const finished = Drive(done, deadline + HardStopSlack, lowest);
            out.gotSome = out.gotSome || parser->got_some();
            if (!finished)
            {
                out.step = Aborted("waiting for the response");
                return;
            }
            if (opEc)
            {
                out.step = Map(opEc, "reading the response");
                return;
            }

            // 101 ends HTTP on this connection: it is final, and never kept.
            unsigned const status = parser->get().result_int();
            if (status / 100 != 1 || status == 101)
                break;
            if (++interims > MaxInterimResponses)
            {
                out.step = {BrainError::BadResponse, "too many interim (1xx) responses"};
                return;
            }
        }

        out.status = int32(parser->get().result_int());
        // Reused only after a clean final response: parser keep_alive() is false when the body ended at EOF, and bytes
        // left in the buffer would belong to a response nobody asked for.
        out.keepAlive = interims == 0 && out.status >= 200 && parser->keep_alive() && buffer.size() == 0;
        out.body = std::move(parser->get().body());
    }

    void Post(BrainHttpRequest const& request, BrainHttpResponse& out)
    {
        if (interrupted.load())
        {
            out.transport = BrainError::Cancelled;
            out.detail = "interrupted (shutdown)";
            return;
        }
        if (!request.url)
        {
            out.transport = BrainError::Network;
            out.detail = "no URL";
            return;
        }

        BrainUrl const& url = *request.url;
        Clock::time_point const deadline = Clock::now() + Ms(request.totalTimeoutMs);

        boost::system::error_code ec;
        asio::ip::address const address = asio::ip::make_address(request.address, ec);
        if (ec)
        {
            out.transport = BrainError::Network;
            out.detail = Excerpt("bad address '" + request.address + "'");
            return;
        }
        asio::ip::tcp::endpoint const endpoint(address, url.port);

        if (url.tls && (!ctx || !tlsReady))
        {
            out.transport = BrainError::Network;
            out.detail = Excerpt("TLS unavailable: " + tlsProblem);
            return;
        }

        // Beast refuses header values with control characters by throwing; say so plainly instead.
        for (char c : request.authorization)
        {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            {
                out.transport = BrainError::Network;
                out.detail = "the API key holds control characters";
                return;
            }
        }

        std::string hostHeader = url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host;
        if (url.port != (url.tls ? 443 : 80))
            hostHeader += ":" + std::to_string(url.port);

        http::request<http::string_body> message{http::verb::post, url.target, 11};
        message.set(http::field::host, hostHeader);
        message.set(http::field::user_agent, UserAgent);
        message.set(http::field::content_type, "application/json");
        message.set(http::field::accept, "application/json");
        if (!request.authorization.empty())
            message.set(request.authHeader, request.authorization);
        for (auto const& [name, value] : request.extraHeaders)
        {
            bool const token = !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       std::string_view("!#$%&'*+-.^_`|~").find(char(c)) != std::string_view::npos;
            });
            if (!token || value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos)
            {
                out.transport = BrainError::BadResponse;
                out.detail = "bad header";
                return;
            }
            message.set(name, value);
        }
        message.keep_alive(true);
        message.body() = request.body;
        message.prepare_payload();

        std::string const key = url.host + ":" + std::to_string(url.port) + (url.tls ? ":tls" : ":tcp");

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            Connection conn;
            bool reused = false;
            auto it = idle.find(key);
            if (it != idle.end())
            {
                if (attempt == 0)
                {
                    conn = std::move(it->second);
                    reused = true;
                }
                idle.erase(it);
            }

            if (!reused)
            {
                StepResult opened = Open(conn, url, endpoint, std::max<uint32>(request.connectTimeoutMs, 1), deadline);
                if (opened.error != BrainError::None)
                {
                    out.transport = opened.error;
                    out.detail = std::move(opened.detail);
                    return;
                }
            }

            Exchanged ex;
            if (conn.tls)
                Exchange(*conn.tls, conn.Lowest(), message, deadline, ex);
            else
                Exchange(*conn.plain, conn.Lowest(), message, deadline, ex);

            if (ex.step.error == BrainError::None)
            {
                out.transport = BrainError::None;
                out.status = ex.status;
                out.body = std::move(ex.body);
                out.reused = reused;
                out.sent = true;
                if (ex.keepAlive && !interrupted.load())
                {
                    conn.Lowest().expires_never();
                    conn.lastUsedMs = CoaTime::NowMs();
                    idle[key] = std::move(conn);
                }
                return;
            }

            // Retry only before writing starts: an unanswered request may already have been billed.
            bool const retry = reused && !ex.sent && !ex.gotSome && !interrupted.load() &&
                               Clock::now() + RetryMinLeft <= deadline;
            if (!retry)
            {
                out.transport = ex.step.error;
                out.detail = std::move(ex.step.detail);
                out.reused = reused;
                out.sent = ex.sent;
                return;
            }
        }
    }
};

bool ParseBrainUrl(std::string_view text, BrainUrl& out, std::string& error)
{
    out = BrainUrl();
    error.clear();

    for (char c : text)
    {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F)
        {
            error = "spaces or control characters in the URL";
            return false;
        }
    }
    if (text.find('#') != std::string_view::npos)
    {
        error = "a fragment (#) is not allowed";
        return false;
    }

    std::size_t const sep = text.find("://");
    if (sep == std::string_view::npos)
    {
        error = "the URL must start with http:// or https://";
        return false;
    }

    std::string const scheme = Lower(text.substr(0, sep));
    if (scheme == "https")
    {
        out.tls = true;
        out.port = 443;
    }
    else if (scheme == "http")
    {
        out.tls = false;
        out.port = 80;
    }
    else
    {
        error = "the scheme must be http or https";
        return false;
    }

    std::string_view const rest = text.substr(sep + 3);
    std::size_t const pathAt = rest.find_first_of("/?");
    std::string_view const authority = rest.substr(0, pathAt);
    if (pathAt == std::string_view::npos)
        out.target = "/";
    else if (rest[pathAt] == '?')
        out.target = "/" + std::string(rest.substr(pathAt));
    else
        out.target = std::string(rest.substr(pathAt));

    if (authority.find('@') != std::string_view::npos)
    {
        error = "user information (user@host) is not allowed";
        return false;
    }
    if (authority.empty())
    {
        error = "empty host";
        return false;
    }

    std::string_view host;
    std::string_view portText;
    bool hasPort = false;
    if (authority.front() == '[')
    {
        std::size_t const close = authority.find(']');
        if (close == std::string_view::npos)
        {
            error = "unclosed [ in the host";
            return false;
        }
        host = authority.substr(1, close - 1);
        std::string_view const after = authority.substr(close + 1);
        if (!after.empty())
        {
            if (after.front() != ':')
            {
                error = "unexpected text after the IPv6 address";
                return false;
            }
            hasPort = true;
            portText = after.substr(1);
        }

        boost::system::error_code ec;
        boost::asio::ip::make_address_v6(std::string(host), ec);
        if (ec)
        {
            error = "bad IPv6 address";
            return false;
        }
    }
    else
    {
        std::size_t const colon = authority.find(':');
        if (colon != std::string_view::npos)
        {
            if (authority.find(':', colon + 1) != std::string_view::npos)
            {
                error = "an IPv6 address must be written in brackets";
                return false;
            }
            host = authority.substr(0, colon);
            hasPort = true;
            portText = authority.substr(colon + 1);
        }
        else
            host = authority;

        for (char c : host)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_')
            {
                error = "bad character in the host name";
                return false;
            }
        }
    }

    if (host.empty())
    {
        error = "empty host";
        return false;
    }

    if (hasPort)
    {
        if (portText.empty() || portText.size() > 5)
        {
            error = "bad port";
            return false;
        }
        uint32 port = 0;
        for (char c : portText)
        {
            if (c < '0' || c > '9')
            {
                error = "bad port";
                return false;
            }
            port = port * 10 + uint32(c - '0');
        }
        if (port < 1 || port > 65535)
        {
            error = "the port must be 1 to 65535";
            return false;
        }
        out.port = uint16(port);
    }

    out.host = Lower(host);

    boost::system::error_code ec;
    boost::asio::ip::address const address = boost::asio::ip::make_address(out.host, ec);
    out.hostIsIp = !ec;
    out.loopback = out.host == "localhost" || (out.hostIsIp && address.is_loopback());
    return true;
}

BrainHttpClient::BrainHttpClient(BrainTlsSettings const& tls)
{
    try
    {
        impl = std::make_unique<Impl>();
        impl->verify = tls.verify;
        impl->ctx = std::make_unique<ssl::context>(ssl::context::tls_client);

        boost::system::error_code ec;
        impl->ctx->set_options(ssl::context::default_workarounds | ssl::context::no_compression, ec);
        SSL_CTX_set_min_proto_version(impl->ctx->native_handle(), TLS1_2_VERSION);
        impl->ctx->set_verify_mode(tls.verify ? ssl::verify_peer : ssl::verify_none, ec);

        if (!tls.caBundle.empty())
        {
            impl->ctx->load_verify_file(tls.caBundle, ec);
            if (ec)
            {
                impl->tlsProblem = Excerpt("cannot load CaBundle " + tls.caBundle + ": " + ec.message());
                return;
            }
        }
        else
        {
            boost::system::error_code pathsEc;
            impl->ctx->set_default_verify_paths(pathsEc);
            std::string storeError;
            uint32 const added = BrainCertStore::AddSystemRoots(impl->ctx->native_handle(), storeError);
            // Windows: OpenSSL's default paths are empty there, so the system store is the only source of roots.
            if (tls.verify && added == 0 && !storeError.empty())
            {
                impl->tlsProblem =
                    Excerpt("no trusted root certificates (" + storeError + "); set AiPlayerbot.CoaBrain.CaBundle");
                return;
            }
            if (tls.verify && pathsEc && added == 0)
            {
                impl->tlsProblem = Excerpt("no trusted root certificates (" + pathsEc.message() +
                                           "); set AiPlayerbot.CoaBrain.CaBundle");
                return;
            }
        }
        impl->tlsReady = true;
    }
    catch (std::exception const& e)
    {
        if (impl)
        {
            impl->ctx.reset();
            impl->tlsReady = false;
            impl->tlsProblem = Excerpt(std::string("TLS setup failed: ") + e.what());
        }
    }
    catch (...)
    {
        if (impl)
        {
            impl->ctx.reset();
            impl->tlsReady = false;
            impl->tlsProblem = "TLS setup failed";
        }
    }
}

BrainHttpClient::~BrainHttpClient() = default;

bool BrainHttpClient::TlsReady() const { return impl && impl->tlsReady; }

std::string const& BrainHttpClient::TlsProblem() const
{
    static std::string const noClient = "the HTTP client could not be created";
    return impl ? impl->tlsProblem : noClient;
}

BrainHttpResponse BrainHttpClient::Post(BrainHttpRequest const& request)
{
    BrainHttpResponse response;
    Clock::time_point const start = Clock::now();

    if (!impl)
    {
        response.transport = BrainError::Network;
        response.detail = "the HTTP client could not be created";
        return response;
    }

    try
    {
        impl->Post(request, response);
    }
    catch (std::exception const& e)
    {
        response.transport = BrainError::Network;
        response.status = 0;
        response.body.clear();
        response.detail = Excerpt(std::string("exception: ") + e.what());
        response.sent = true;  // unknown: never sent again elsewhere
    }
    catch (...)
    {
        response.transport = BrainError::Network;
        response.status = 0;
        response.body.clear();
        response.detail = "exception";
        response.sent = true;
    }

    response.ms = uint32(std::chrono::duration_cast<Ms>(Clock::now() - start).count());
    return response;
}

void BrainHttpClient::Interrupt()
{
    if (!impl)
        return;
    impl->interrupted.store(true);
    impl->ioc.stop();
}

void BrainHttpClient::CloseIdle(uint32 idleMs)
{
    if (!impl)
        return;

    uint64 const now = CoaTime::NowMs();
    for (auto it = impl->idle.begin(); it != impl->idle.end();)
    {
        if (now - it->second.lastUsedMs > idleMs)
            it = impl->idle.erase(it);
        else
            ++it;
    }
}
