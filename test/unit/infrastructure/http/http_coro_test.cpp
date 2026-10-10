//
// Unit tests for the coroutine HTTP protocol layer, driven by a scripted
// in-memory stream (the net::Stream injection point).
//
// Covers: framing per framing style, 1xx interim handling, the upgrade reject,
// keep-alive carry-over, the lazy chunked read window, the redirect policy,
// transport connection setup and the session close deferral.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "infrastructure/http/protocol/exchange.h"
#include "infrastructure/http/protocol/read_window.h"
#include "infrastructure/http/protocol/wire.h"
#include "infrastructure/http/redirect.h"
#include "infrastructure/http/session.h"
#include "infrastructure/http/transport.h"
#include "infrastructure/http/wire_request.h"
#include "infrastructure/network/transport/stream.h"
#include "infrastructure/uri/uri.h"

namespace {

using http::ErrorCode;
using http::protocol::ReadWindow;

/// A scripted in-memory stream: serves the canned response in `chunk`-sized
/// reads and records everything written to it.
class ScriptedStream : public net::Stream {
public:
    explicit ScriptedStream(std::string incoming, const std::size_t chunk = 4096)
        : incoming_(std::move(incoming)), chunk_(chunk) {}

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override { co_return {}; }

    [[nodiscard]] coro::Task<std::expected<std::size_t, net::IoError>> read_some(
        const std::span<std::uint8_t> buf) override {
        if (cursor_ >= incoming_.size()) {
            co_return std::size_t{0};  // EOF
        }
        const std::size_t available = incoming_.size() - cursor_;
        const std::size_t count = std::min({buf.size(), chunk_, available});
        std::memcpy(buf.data(), incoming_.data() + cursor_, count);
        cursor_ += count;
        co_return count;
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> read_exact(const std::span<std::uint8_t> buf) override {
        std::size_t filled = 0;
        while (filled < buf.size()) {
            auto read = co_await read_some(buf.subspan(filled));
            if (!read) {
                co_return std::unexpected(read.error());
            }
            if (*read == 0) {
                co_return std::unexpected(net::IoError::CONNECTION_FAILED);
            }
            filled += *read;
        }
        co_return {};
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> send_all(
        const std::span<const std::uint8_t> data) override {
        sent_.append(reinterpret_cast<const char*>(data.data()), data.size());
        co_return {};
    }

    void close() noexcept override {}

    [[nodiscard]] bool connected() const noexcept override { return true; }

    [[nodiscard]] const std::string& sent() const noexcept { return sent_; }

private:
    std::string incoming_;
    std::size_t chunk_;
    std::size_t cursor_{0};
    std::string sent_;
};

[[nodiscard]] http::protocol::WireRequest get_request(std::string target = "/index.html") {
    http::protocol::WireRequest request;
    request.method = http::Method::GET;
    request.target = std::move(target);
    request.headers.emplace("Host", "example.test");
    return request;
}

/// Run one exchange against a scripted stream.
[[nodiscard]] std::expected<http::protocol::RawResponse, http::Error> run_exchange(
    ScriptedStream& stream, const http::protocol::WireRequest& request, std::string& pending,
    const http::Limits& limits = {}) {
    return coro::run([&]() -> coro::Task<std::expected<http::protocol::RawResponse, http::Error>> {
        co_return co_await http::protocol::exchange(stream, request, limits, pending);
    }());
}

TEST(HttpResolveHost, Literal_WithoutResolver_ReturnsAddress) {
    const http::Options options;
    const auto result = coro::run(http::resolve_host("203.0.113.10", options));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->front().to_string(), "203.0.113.10");
}

TEST(HttpResolveHost, Hostname_WithoutResolver_FailsExplicitly) {
    const http::Options options;
    const auto result = coro::run(http::resolve_host("example.test", options));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::RESOLVE_FAILED);
    EXPECT_EQ(result.error().message, "no hostname resolver configured");
}

TEST(HttpResolveHost, InjectedResolver_ReceivesHostnameAndFamily) {
    http::Options options;
    options.address_family = domain::AddressFamily::IPV6;
    int calls = 0;
    options.resolve = [&calls](std::string host, std::optional<domain::AddressFamily> family)
        -> coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> {
        ++calls;
        EXPECT_EQ(host, "example.test");
        EXPECT_EQ(family, domain::AddressFamily::IPV6);
        co_return std::vector<domain::InetAddress>{*domain::InetAddress::parse("2001:db8::10")};
    };
    const auto result = coro::run(http::resolve_host("example.test", options));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->front().to_string(), "2001:db8::10");
    EXPECT_EQ(calls, 1);
    const auto literal = coro::run(http::resolve_host("203.0.113.10", options));
    EXPECT_TRUE(literal.has_value());
    EXPECT_EQ(calls, 1);
}

TEST(HttpResolveHost, ResolverFailure_PreservesDiagnostic) {
    http::Options options;
    options.resolve = [](std::string, std::optional<domain::AddressFamily>)
        -> coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::NX_DOMAIN, "injected NXDOMAIN"});
    };
    const auto result = coro::run(http::resolve_host("example.test", options));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::RESOLVE_FAILED);
    EXPECT_EQ(result.error().message, "injected NXDOMAIN");
}

TEST(HttpResolveHost, ResolverCancellation_Propagates) {
    http::Options options;
    coro::CancelScope* active_scope = nullptr;
    options.resolve = [&active_scope](std::string, std::optional<domain::AddressFamily>)
        -> coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> {
        active_scope->cancel();
        co_await coro::checkpoint();
        co_return std::vector<domain::InetAddress>{};
    };
    bool returned = false;
    coro::run([&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            active_scope = &scope;
            [[maybe_unused]] const auto result = co_await http::resolve_host("example.test", options);
            returned = true;
        });
        EXPECT_TRUE(outcome.cancelled);
    }());
    EXPECT_FALSE(returned);
}

TEST(HttpWireRequest, ConnectionError_PreservesCodeAndStageMessage) {
    const auto error = http::connection_error("GET /index.html");
    EXPECT_EQ(error.code, ErrorCode::CONNECTION_LOST);
    EXPECT_EQ(error.message, "GET /index.html: connection lost");
}

TEST(HttpWireRequest, ConnectError_PreservesCodeAndMessage) {
    const auto error = http::connect_error();
    EXPECT_EQ(error.code, ErrorCode::CONNECT_FAILED);
    EXPECT_EQ(error.message, "connect failed");
}

// ---------------------------------------------------------------------------
// response framing
// ---------------------------------------------------------------------------

TEST(HttpExchange, content_length_body_DecodesAndReadsPastTheBuffer) {
    ScriptedStream stream{"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->text(), "hello");
    // Bytes past the body belong to the next response, not to this one.
    EXPECT_EQ(pending, "EXTRA");
    EXPECT_TRUE(stream.sent().starts_with("GET /index.html HTTP/1.1\r\n"));
}

TEST(HttpExchange, chunked_body_WithExtensionsAndTrailers_Decodes) {
    ScriptedStream stream{
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5;ext=\"v\"\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\nX-Trailer: yes\r\n\r\n"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->text(), "hello world");
    ASSERT_EQ(response->trailers.size(), 1U);
    EXPECT_EQ(response->trailers.begin()->first, "X-Trailer");
    EXPECT_EQ(response->trailers.begin()->second, "yes");
    // HTTP/1.1 without a Connection: close is reusable.
}

TEST(HttpExchange, chunked_manyOneByteReads_DecodesIdentically) {
    // 4000 one-byte chunks, each delivered as its own one-byte read, so every
    // incremental branch of the decoder runs and the read window is exercised
    // with the smallest possible steps.
    std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    std::string expected;
    for (int i = 0; i < 4000; ++i) {
        const char digit = static_cast<char>('a' + (i % 26));
        wire += "1\r\n";
        wire += digit;
        wire += "\r\n";
        expected += digit;
    }
    wire += "0\r\n\r\n";

    ScriptedStream stream{std::move(wire), 1};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->size(), expected.size());
    EXPECT_EQ(response->text(), expected);
}

TEST(HttpExchange, interim_100Continue_IsConsumedBeforeTheFinalResponse) {
    ScriptedStream stream{
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->text(), "ok");
}

TEST(HttpExchange, upgrade_101_IsRejected) {
    ScriptedStream stream{"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::UNSUPPORTED_PROTOCOL);
}

TEST(HttpExchange, noBody_204_LeavesTheBodyEmptyAndStaysReusable) {
    ScriptedStream stream{"HTTP/1.1 204 No Content\r\n\r\n"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 204);
    EXPECT_TRUE(response->text().empty());
    // 204 has no body framing, and the request did not ask to close, so the
    // connection is reusable under HTTP/1.1.
    EXPECT_TRUE(response->reusable);
}

TEST(HttpExchange, keepAlive_twoResponsesInOneRead_CarryOverThroughPending) {
    ScriptedStream stream{
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none"
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo"};
    std::string pending;

    const auto first = run_exchange(stream, get_request("/first"), pending);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->text(), "one");
    EXPECT_TRUE(pending.starts_with("HTTP/1.1 200 OK"));

    const auto second = run_exchange(stream, get_request("/second"), pending);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->text(), "two");
    EXPECT_TRUE(pending.empty());
    EXPECT_TRUE(second->reusable);
}

TEST(HttpExchange, conflicting_framing_IsRejected) {
    ScriptedStream stream{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(HttpExchange, headers_overTheLimit_Fail) {
    std::string wire = "HTTP/1.1 200 OK\r\nX-Filler: ";
    wire += std::string(200, 'a');
    wire += "\r\nContent-Length: 0\r\n\r\n";

    ScriptedStream stream{std::move(wire)};
    std::string pending;
    http::Limits limits;
    limits.max_header_bytes = 64;
    const auto response = run_exchange(stream, get_request(), pending, limits);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::HEADERS_TOO_LARGE);
}

// ---------------------------------------------------------------------------
// the lazy read window
// ---------------------------------------------------------------------------

TEST(ReadWindow, consumingTinyIncrements_KeepsTheLiveBufferBounded) {
    // One byte appended and one byte consumed per step, 100000 times: the live
    // buffer must never grow with the amount consumed, which is what makes the
    // decoder linear instead of quadratic.
    ReadWindow window{"x"};
    std::size_t max_live = 0;
    for (int i = 0; i < 100000; ++i) {
        window.append("y", 1);
        window.consume(1);
        max_live = std::max(max_live, window.capacity_in_use());
    }

    EXPECT_EQ(window.size(), 1U);  // the first byte was never consumed
    EXPECT_GE(window.compacted_bytes(), 100000U - 2 * ReadWindow::COMPACT_THRESHOLD);  // dropped, not copied
    EXPECT_LT(max_live, 2 * ReadWindow::COMPACT_THRESHOLD + 8);
}

TEST(ReadWindow, take_rest_EmptiesTheWindow) {
    ReadWindow window{"abcdef"};
    window.consume(3);
    EXPECT_EQ(window.take_rest(), "def");
    EXPECT_EQ(window.size(), 0U);
    EXPECT_TRUE(window.view().empty());
}

// ---------------------------------------------------------------------------
// redirect policy
// ---------------------------------------------------------------------------

TEST(HttpRedirect, moved_permanently_RewritesToGetAndKeepsTheOrigin) {
    const auto current = get_request("/old");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;

    const auto evaluation = http::evaluate_redirect(301, {{"Location", "/new?q=1"}}, 0, options, current, uri);

    ASSERT_TRUE(evaluation.plan.has_value());
    EXPECT_FALSE(evaluation.plan->cross_origin);
    EXPECT_EQ(evaluation.plan->next.method, http::Method::GET);
    EXPECT_EQ(evaluation.plan->next.target, "/new?q=1");
    EXPECT_FALSE(evaluation.plan->next.body.has_value());
}

TEST(HttpRedirect, temporary_redirect_307_PreservesMethodAndBody) {
    auto current = get_request("/old");
    current.method = http::Method::POST;
    current.body = "payload";
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;

    const auto evaluation =
        http::evaluate_redirect(307, {{"Location", "https://example.test/new"}}, 0, options, current, uri);

    ASSERT_TRUE(evaluation.plan.has_value());
    ASSERT_TRUE(evaluation.plan->next.body.has_value());
    EXPECT_EQ(*evaluation.plan->next.body, "payload");
    EXPECT_EQ(evaluation.plan->next.method, http::Method::POST);
    ASSERT_EQ(evaluation.plan->next.headers.count("Content-Length"), 1U);
}

TEST(HttpRedirect, cross_origin_DropsCredentials) {
    auto current = get_request("/old");
    current.headers.emplace("Authorization", "Bearer secret");
    current.headers.emplace("Cookie", "session=1");
    current.headers.emplace("X-Keep", "yes");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;

    const auto evaluation =
        http::evaluate_redirect(302, {{"Location", "https://other.test/new"}}, 0, options, current, uri);

    ASSERT_TRUE(evaluation.plan.has_value());
    EXPECT_TRUE(evaluation.plan->cross_origin);
    EXPECT_EQ(evaluation.plan->next.headers.count("Authorization"), 0U);
    EXPECT_EQ(evaluation.plan->next.headers.count("Cookie"), 0U);
    EXPECT_EQ(evaluation.plan->next.headers.count("X-Keep"), 1U);
}

TEST(HttpRedirect, https_to_http_downgrade_IsNotFollowed) {
    const auto current = get_request("/old");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;

    const auto evaluation =
        http::evaluate_redirect(302, {{"Location", "http://example.test/new"}}, 0, options, current, uri);

    EXPECT_FALSE(evaluation.plan.has_value());
    EXPECT_FALSE(evaluation.limit_reached);
}

TEST(HttpRedirect, beyondTheLimit_ReportsTheLimit) {
    const auto current = get_request("/old");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;
    options.max_redirects = 2;

    const auto evaluation = http::evaluate_redirect(302, {{"Location", "/new"}}, 2, options, current, uri);

    EXPECT_FALSE(evaluation.plan.has_value());
    EXPECT_TRUE(evaluation.limit_reached);
}

TEST(HttpRedirect, disabledFollowing_LeavesTheResponseAlone) {
    const auto current = get_request("/old");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;
    options.follow_redirects = false;

    const auto evaluation = http::evaluate_redirect(302, {{"Location", "/new"}}, 0, options, current, uri);

    EXPECT_FALSE(evaluation.plan.has_value());
    EXPECT_FALSE(evaluation.limit_reached);
}

// ---------------------------------------------------------------------------
// connection setup (transport): the TLS identity defaults to the origin host
// ---------------------------------------------------------------------------

/// A factory that records what the transport asked for and serves a scripted
/// stream instead of opening a socket.
class CapturingFactory final : public net::StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&,
                                                          const net::TlsOptions& tls_options,
                                                          std::shared_ptr<const net::TlsContext>) override {
        sni = tls_options.sni_hostname;
        ++tls_calls;
        return std::make_unique<ScriptedStream>("HTTP/1.1 204 No Content\r\n\r\n");
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        ++tcp_calls;
        return std::make_unique<ScriptedStream>("HTTP/1.1 204 No Content\r\n\r\n");
    }

    std::optional<std::string> sni;
    int tls_calls{0};
    int tcp_calls{0};
};

/// Connect through the capturing factory and return the resulting stream.
[[nodiscard]] std::expected<std::unique_ptr<net::Stream>, http::Error> run_connect(const std::string_view scheme,
                                                                                   const std::string_view host,
                                                                                   const domain::InetAddress& address,
                                                                                   const http::Options& options) {
    return coro::run([&]() -> coro::Task<std::expected<std::unique_ptr<net::Stream>, http::Error>> {
        co_return co_await http::connect_stream(scheme, host, std::span(&address, 1), 443, options);
    }());
}

TEST(HttpConnectStream, https_WithoutPinnedName_DefaultsSniToTheOriginHost) {
    auto factory = std::make_shared<CapturingFactory>();
    http::Options options;
    options.factory = factory;
    const auto address = domain::InetAddress::parse("203.0.113.10").value();

    const auto stream = run_connect("https", "example.test", address, options);

    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(factory->tls_calls, 1);
    ASSERT_TRUE(factory->sni.has_value());
    EXPECT_EQ(*factory->sni, "example.test");
}

TEST(HttpConnectStream, https_WithPinnedName_KeepsTheExplicitSni) {
    auto factory = std::make_shared<CapturingFactory>();
    http::Options options;
    options.factory = factory;
    options.tls.sni_hostname = "pinned.test";
    const auto address = domain::InetAddress::parse("203.0.113.10").value();

    const auto stream = run_connect("https", "example.test", address, options);

    ASSERT_TRUE(stream.has_value());
    ASSERT_TRUE(factory->sni.has_value());
    EXPECT_EQ(*factory->sni, "pinned.test");
}

TEST(HttpConnectStream, https_WithIpLiteralHost_NeverSetsSni) {
    auto factory = std::make_shared<CapturingFactory>();
    http::Options options;
    options.factory = factory;
    const auto address = domain::InetAddress::parse("203.0.113.10").value();

    const auto stream = run_connect("https", "203.0.113.10", address, options);

    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(factory->tls_calls, 1);
    // RFC 6066 §3 forbids an IP literal in SNI, so nothing is filled in.
    EXPECT_FALSE(factory->sni.has_value());
}

TEST(HttpConnectStream, https_WithIpv6LiteralHost_NeverSetsSni) {
    auto factory = std::make_shared<CapturingFactory>();
    http::Options options;
    options.factory = factory;
    const auto address = domain::InetAddress::parse("2001:db8::10").value();

    const auto stream = run_connect("https", "2001:db8::10", address, options);

    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(factory->tls_calls, 1);
    EXPECT_FALSE(factory->sni.has_value());
}

TEST(HttpConnectStream, plain_http_OpensTcpAndNeverTouchesTheHostName) {
    auto factory = std::make_shared<CapturingFactory>();
    http::Options options;
    options.factory = factory;
    const auto address = domain::InetAddress::parse("203.0.113.10").value();

    const auto stream = run_connect("http", "example.test", address, options);

    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(factory->tcp_calls, 1);
    EXPECT_EQ(factory->tls_calls, 0);
    EXPECT_FALSE(factory->sni.has_value());
}

// ---------------------------------------------------------------------------
// session: a close() that lands while another exchange holds the guard
// ---------------------------------------------------------------------------

/// A scripted stream whose connect check parks for one clock tick, so a test
/// can act while an exchange holds the session guard inside ensure_stream().
class SlowConnectStream final : public ScriptedStream {
public:
    using ScriptedStream::ScriptedStream;

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override {
        co_await coro::sleep_for(std::chrono::milliseconds{1});
        // A resume after a mid-exchange teardown lands here, on the stream the
        // exchange still borrows.
        ++connect_checks;
        co_return {};
    }

    int connect_checks{0};
};

/// Serves one queued response payload per TCP connection, on slow-connect
/// streams, and counts the connections.
class ScriptedSessionFactory final : public net::StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&, const net::TlsOptions&,
                                                          std::shared_ptr<const net::TlsContext>) override {
        ++tls_calls;
        return std::make_unique<SlowConnectStream>("");
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        ++tcp_calls;
        std::string payload;
        if (!payloads.empty()) {
            payload = std::move(payloads.front());
            payloads.pop_front();
        }
        return std::make_unique<SlowConnectStream>(std::move(payload));
    }

    std::deque<std::string> payloads;
    int tls_calls{0};
    int tcp_calls{0};
};

// Regression test for the DoH teardown crash: a close() landing while a second
// exchange holds the session guard must not tear down the stream that exchange
// borrowed; the close is deferred to the guard's checkpoint instead. With the
// manual clock the second exchange is parked inside the connection's connect
// check when the close lands; the legacy unconditional close() destroyed the
// borrowed stream there (a use-after-free on resume, seen under ASan).
TEST(HttpSession, close_DuringQueuedExchange_DefersTeardownUntilCompletion) {
    auto factory = std::make_shared<ScriptedSessionFactory>();
    factory->payloads = {
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none"
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nthree"};
    http::Options options;
    options.factory = factory;
    http::Session session{std::move(options), "http", "203.0.113.10", 8080};
    const http::Request request;

    std::optional<http::Response> first_response;
    std::optional<http::Response> second_response;
    std::optional<http::Response> third_response;
    int calls_before_third = 0;
    coro::ManualClock clock;
    coro::Loop loop{clock};

    auto task = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            auto first = group.spawn(session.exchange("/first", request));
            auto second = group.spawn(session.exchange("/second", request));
            // 2ms: the first exchange has completed on connection 1 and the
            // second holds the session guard, parked in the connection's
            // connect check. The close lands mid-exchange.
            co_await coro::sleep_for(std::chrono::milliseconds{2});
            session.close();
            auto first_result = co_await first;
            auto second_result = co_await second;
            calls_before_third = factory->tcp_calls;
            if (first_result.has_value()) {
                first_response.emplace(std::move(*first_result));
            }
            if (second_result.has_value()) {
                second_response.emplace(std::move(*second_result));
            }
            auto third_result = co_await session.exchange("/third", request);
            if (third_result.has_value()) {
                third_response.emplace(std::move(*third_result));
            }
            co_return;
        });
        co_return;
    };

    coro::run(loop, task());
    ASSERT_TRUE(first_response.has_value());
    EXPECT_EQ(first_response->text(), "one");
    ASSERT_TRUE(second_response.has_value());
    EXPECT_EQ(second_response->text(), "two");
    // The second exchange reused connection 1; the deferred close dropped it
    // only when that exchange completed.
    EXPECT_EQ(calls_before_third, 1);
    ASSERT_TRUE(third_response.has_value());
    EXPECT_EQ(third_response->text(), "three");
    EXPECT_EQ(factory->tcp_calls, 2);
}

}  // namespace
