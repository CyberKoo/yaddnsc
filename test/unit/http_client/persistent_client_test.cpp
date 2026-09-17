//
// Unit tests for net::http::PersistentClient
// (src/infrastructure/network/http/persistent_client.cpp) and the shared wire-request
// helpers (src/infrastructure/network/http/wire_request.cpp).
//
// Request-target semantics and redirect handling (same-origin follow-up on
// the persistent connection, cross-origin transient hop, redirect limit)
// are driven through a scripted in-memory StreamFactory; the wire helpers
// are pure functions (no network I/O).
// =============================================================================

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cstring>
#include <expected>
#include <gtest/gtest.h>

#include "infrastructure/network/http/client.h"
#include "infrastructure/network/http/persistent_client.h"
#include "infrastructure/network/http/redirect.h"
#include "infrastructure/network/http/wire_request.h"
#include "infrastructure/network/transport/stream.h"

using net::http::ErrorCode;
using net::http::Method;
using Transport::IoError;

namespace {

class FakeStream final : public Transport::Stream {
public:
    std::string input;
    std::optional<IoError> fail_reads;
    std::optional<IoError> connect_error;
    std::string sent;
    /// Max bytes returned per read_some call. Useful to model a server that
    /// does not make a subsequent response available before receiving the
    /// next request.
    size_t max_chunk = 0;

    [[nodiscard]] std::expected<void, IoError> ensure_connected() override {
        if (connect_error) {
            return std::unexpected(*connect_error);
        }
        return {};
    }

    void close() noexcept override {}

    [[nodiscard]] std::expected<size_t, IoError> read_some(std::span<std::uint8_t> buf) override {
        if (fail_reads) {
            return std::unexpected(*fail_reads);
        }
        if (pos_ >= input.size()) {
            return std::unexpected(IoError::CONNECTION_FAILED);  // EOF
        }
        size_t n = std::min(buf.size(), input.size() - pos_);
        if (max_chunk > 0) {
            n = std::min(n, max_chunk);
        }
        std::memcpy(buf.data(), input.data() + pos_, n);
        pos_ += n;
        return n;
    }

    [[nodiscard]] std::expected<void, IoError> read_exact(std::span<std::uint8_t> buf) override {
        auto remaining = buf;
        while (!remaining.empty()) {
            auto n = read_some(remaining);
            if (!n) {
                return std::unexpected(n.error());
            }
            remaining = remaining.subspan(*n);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, IoError> send_all(std::span<const std::uint8_t> data) override {
        sent.append(reinterpret_cast<const char*>(data.data()), data.size());
        return {};
    }

private:
    size_t pos_ = 0;
};

std::unique_ptr<FakeStream> stream_responding(std::string raw) {
    auto stream = std::make_unique<FakeStream>();
    stream->input = std::move(raw);
    return stream;
}

std::unique_ptr<FakeStream> ok_stream(std::string body_marker = "done") {
    return stream_responding("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body_marker.size()) +
                             "\r\n\r\n" + body_marker);
}

/// Factory serving scripted streams per host, recording every call.
class FakeFactory final : public net::http::StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<Transport::Stream>
        create_tls(std::string_view host, std::uint16_t /*port*/, const Transport::Options& /*conn_opts*/,
                   const Transport::TlsOptions& /*tls_opts*/) override {
        tls_hosts.emplace_back(host);
        return next(tls_streams);
    }

    [[nodiscard]] std::unique_ptr<Transport::Stream>
        create_tcp(std::string_view host, std::uint16_t /*port*/, const Transport::Options& /*opts*/) override {
        tcp_hosts.emplace_back(host);
        return next(tcp_streams);
    }

    std::deque<std::unique_ptr<FakeStream>> tcp_streams;
    std::deque<std::unique_ptr<FakeStream>> tls_streams;
    std::vector<std::string> tcp_hosts;
    std::vector<std::string> tls_hosts;

private:
    static std::unique_ptr<Transport::Stream> next(std::deque<std::unique_ptr<FakeStream>>& streams) {
        if (streams.empty()) {
            return std::make_unique<FakeStream>();  // blank stream: EOF on read
        }
        auto stream = std::move(streams.front());
        streams.pop_front();
        return stream;
    }
};

net::http::Request plain_get() {
    net::http::Request req;
    req.method = Method::GET;
    return req;
}

}  // namespace

// ── wire-request helpers (pure functions) ────────────────────────────────────

net::http::protocol::WireRequest redirect_request() {
    net::http::protocol::WireRequest request;
    request.method = Method::POST;
    request.target = "/dir/page";
    request.body = "payload";
    request.headers.emplace("Authorization", "Bearer secret");
    request.headers.emplace("Cookie", "session=abc");
    request.headers.emplace("Proxy-Authorization", "Basic proxy");
    request.headers.emplace("Content-Type", "text/plain");
    request.headers.emplace("X-Keep", "yes");
    return request;
}

std::multimap<std::string, std::string> location_headers(std::string location) {
    return {{"Location", std::move(location)}};
}

TEST(HttpRedirect, IgnoresNonRedirectAndMissingOrDisabledLocation) {
    const auto request = redirect_request();
    const auto current = Uri::parse("http://example.test:8080/dir/page");
    net::http::Options opts;

    EXPECT_FALSE(net::http::evaluate_redirect(200, location_headers("/next"), 0, opts, request, current).plan);
    EXPECT_FALSE(net::http::evaluate_redirect(302, {}, 0, opts, request, current).plan);

    opts.follow_redirects = false;
    EXPECT_FALSE(net::http::evaluate_redirect(302, location_headers("/next"), 0, opts, request, current).plan);
}

TEST(HttpRedirect, EnforcesLimitAndRejectsMalformedLocations) {
    const auto request = redirect_request();
    const auto current = Uri::parse("http://example.test/dir/page");
    net::http::Options opts{.max_redirects = 2};

    const auto limited = net::http::evaluate_redirect(302, location_headers("/next"), 2, opts, request, current);
    EXPECT_FALSE(limited.plan);
    EXPECT_TRUE(limited.limit_reached);
    EXPECT_FALSE(net::http::evaluate_redirect(302, location_headers("   "), 0, opts, request, current).plan);
    EXPECT_FALSE(net::http::evaluate_redirect(302, location_headers("http:///missing-host"), 0, opts, request, current).plan);
    EXPECT_FALSE(net::http::evaluate_redirect(302, location_headers("http://[::1/malformed"), 0, opts, request, current).plan);
}

TEST(HttpRedirect, RewritesPostAndPreservesSafeHeadersForSameOrigin) {
    const auto request = redirect_request();
    const auto current = Uri::parse("http://example.test:8080/dir/page");
    const auto result = net::http::evaluate_redirect(303, location_headers("next?x=1"), 0, {}, request, current);

    ASSERT_TRUE(result.plan);
    const auto& plan = *result.plan;
    EXPECT_FALSE(plan.cross_origin);
    EXPECT_EQ(plan.scheme, "http");
    EXPECT_EQ(plan.host, "example.test");
    EXPECT_EQ(plan.port, 8080);
    EXPECT_EQ(plan.next.target, "/dir/next?x=1");
    EXPECT_EQ(plan.next.method, Method::GET);
    EXPECT_FALSE(plan.next.body);
    EXPECT_EQ(plan.next.headers.count("Authorization"), 1);
    EXPECT_EQ(plan.next.headers.count("Cookie"), 1);
    EXPECT_EQ(plan.next.headers.count("Content-Type"), 0);
    EXPECT_EQ(plan.next.headers.find("Host")->second, "example.test:8080");
}

TEST(HttpRedirect, NormalizesDotSegmentsAndDropsFragments) {
    const auto request = redirect_request();
    const auto current = Uri::parse("https://example.test/a/b/page?old=1");

    const auto relative = net::http::evaluate_redirect(302, location_headers("../next#section"), 0, {}, request, current);
    ASSERT_TRUE(relative.plan);
    EXPECT_EQ(relative.plan->next.target, "/a/next");

    const auto query = net::http::evaluate_redirect(302, location_headers("?new=1#section"), 0, {}, request, current);
    ASSERT_TRUE(query.plan);
    EXPECT_EQ(query.plan->next.target, "/a/b/page?new=1");
}

TEST(HttpRedirect, PreservesBodyButDropsCredentialsAcrossOrigins) {
    const auto request = redirect_request();
    const auto current = Uri::parse("https://example.test/start");
    const auto result = net::http::evaluate_redirect(307, location_headers("//[2001:db8::1]:8443/next"), 0, {}, request, current);

    ASSERT_TRUE(result.plan);
    const auto& plan = *result.plan;
    EXPECT_TRUE(plan.cross_origin);
    EXPECT_EQ(plan.scheme, "https");
    EXPECT_EQ(plan.host, "2001:db8::1");
    EXPECT_EQ(plan.port, 8443);
    EXPECT_EQ(plan.next.target, "/next");
    EXPECT_EQ(plan.next.method, Method::POST);
    ASSERT_TRUE(plan.next.body);
    EXPECT_EQ(*plan.next.body, "payload");
    EXPECT_EQ(plan.next.headers.count("Authorization"), 0);
    EXPECT_EQ(plan.next.headers.count("Cookie"), 0);
    EXPECT_EQ(plan.next.headers.count("Proxy-Authorization"), 0);
    EXPECT_EQ(plan.next.headers.find("Host")->second, "[2001:db8::1]:8443");
    EXPECT_EQ(plan.next.headers.find("Content-Length")->second, "7");
    EXPECT_EQ(plan.next.headers.find("Content-Type")->second, "text/plain");
}

TEST(HttpWireRequest, DefaultPort) {
    EXPECT_EQ(net::http::default_port("http"), 80);
    EXPECT_EQ(net::http::default_port("https"), 443);
}

TEST(HttpWireRequest, MakeHostHeader) {
    // Default port is omitted (RFC 7230 §5.4).
    EXPECT_EQ(net::http::make_host_header("http", "example.com", 80), "example.com");
    EXPECT_EQ(net::http::make_host_header("https", "example.com", 443), "example.com");
    // Non-default port is kept.
    EXPECT_EQ(net::http::make_host_header("http", "example.com", 8080), "example.com:8080");
    // IPv6 literals are bracketed.
    EXPECT_EQ(net::http::make_host_header("http", "::1", 80), "[::1]");
    EXPECT_EQ(net::http::make_host_header("http", "::1", 8080), "[::1]:8080");
}

TEST(HttpWireRequest, MakeTarget) {
    EXPECT_EQ(net::http::make_target(Uri::parse("http://a.test")), "/");
    EXPECT_EQ(net::http::make_target(Uri::parse("http://a.test/path")), "/path");
    EXPECT_EQ(net::http::make_target(Uri::parse("http://a.test/path?q=1&r=2")), "/path?q=1&r=2");
}

TEST(HttpWireRequest, BuildWireRequest_NoBody) {
    net::http::Options opts;
    const auto wire = net::http::build_wire_request(plain_get(), "http", "a.test", 80, opts);

    EXPECT_EQ(wire.method, Method::GET);
    EXPECT_EQ(wire.headers.count("Host"), 1);
    EXPECT_EQ(wire.headers.find("Host")->second, "a.test");
    EXPECT_EQ(wire.headers.count("User-Agent"), 0);  // empty UA is not emitted
    EXPECT_EQ(wire.headers.count("Content-Length"), 0);
    EXPECT_FALSE(wire.body.has_value());
}

TEST(HttpWireRequest, BuildWireRequest_WithUserAgent) {
    net::http::Options opts;
    opts.user_agent = "yaddnsc-test";
    const auto wire = net::http::build_wire_request(plain_get(), "http", "a.test", 8080, opts);

    EXPECT_EQ(wire.headers.find("Host")->second, "a.test:8080");
    EXPECT_EQ(wire.headers.find("User-Agent")->second, "yaddnsc-test");
}

TEST(HttpWireRequest, Http10EmitsExplicitConnectionPolicy) {
    net::http::Options opts{.version = net::http::HttpVersion::V1_0};
    auto keep_alive = net::http::build_wire_request(plain_get(), "http", "a.test", 80, opts);
    EXPECT_EQ(keep_alive.headers.find("Connection")->second, "keep-alive");

    opts.keep_alive = false;
    auto close = net::http::build_wire_request(plain_get(), "http", "a.test", 80, opts);
    EXPECT_EQ(close.headers.find("Connection")->second, "close");
}

TEST(HttpWireRequest, ValidationRejectsHeaderInjectionAndBuilderOwnsFraming) {
    auto unsafe = plain_get();
    unsafe.headers.emplace("X-Test", "safe\r\nInjected: yes");
    ASSERT_FALSE(net::http::validate_request(unsafe));

    auto managed = plain_get();
    managed.headers.emplace("Host", "attacker.test");
    managed.headers.emplace("Content-Length", "999");
    managed.headers.emplace("Connection", "close");
    managed.headers.emplace("Transfer-Encoding", "chunked");
    const auto wire = net::http::build_wire_request(managed, "http", "a.test", 80, {});
    EXPECT_EQ(wire.headers.count("Host"), 1);
    EXPECT_EQ(wire.headers.find("Host")->second, "a.test");
    EXPECT_EQ(wire.headers.count("Content-Length"), 0);
    EXPECT_EQ(wire.headers.count("Connection"), 0);
    EXPECT_EQ(wire.headers.count("Transfer-Encoding"), 0);
}

TEST(HttpWireRequest, BuildWireRequest_BodyWithAndWithoutContentType) {
    net::http::Options opts;

    net::http::Request no_ct;
    no_ct.method = Method::POST;
    no_ct.set_body("payload");
    const auto wire1 = net::http::build_wire_request(no_ct, "http", "a.test", 80, opts);
    EXPECT_EQ(wire1.headers.find("Content-Length")->second, "7");
    EXPECT_EQ(wire1.headers.count("Content-Type"), 0);

    net::http::Request with_ct;
    with_ct.method = Method::POST;
    with_ct.set_body("{}");
    with_ct.content_type = "application/json";
    const auto wire2 = net::http::build_wire_request(with_ct, "http", "a.test", 80, opts);
    EXPECT_EQ(wire2.headers.find("Content-Length")->second, "2");
    EXPECT_EQ(wire2.headers.find("Content-Type")->second, "application/json");
}

TEST(HttpWireRequest, MapConnectError) {
    EXPECT_EQ(net::http::map_connect_error(Transport::IoError::CANCELLED).code, ErrorCode::CANCELLED);
    EXPECT_EQ(net::http::map_connect_error(Transport::IoError::TIMEOUT).code, ErrorCode::TIMEOUT);
    EXPECT_EQ(net::http::map_connect_error(Transport::IoError::CONNECTION_FAILED).code, ErrorCode::CONNECT_FAILED);
}

// ── construction validation ──────────────────────────────────────────────────

TEST(HttpPersistentClient, InvalidBaseUrl_Throws) {
    auto factory = std::make_shared<FakeFactory>();
    EXPECT_THROW((net::http::PersistentClient("ftp://a.test", {}, factory)), std::invalid_argument);
    EXPECT_THROW((net::http::PersistentClient("http://", {}, factory)), std::invalid_argument);
    EXPECT_THROW((net::http::PersistentClient("not-a-url", {}, factory)), std::invalid_argument);
}

TEST(HttpPersistentClient, ValidBaseUrl_DefaultPorts) {
    auto factory = std::make_shared<FakeFactory>();
    // Implicit default port: no throw, port resolved internally.
    EXPECT_NO_THROW((net::http::PersistentClient("http://a.test", {}, factory)));
    EXPECT_NO_THROW((net::http::PersistentClient("https://a.test", {}, factory)));
    // IPv6 host: brackets are not part of the host.
    EXPECT_NO_THROW((net::http::PersistentClient("http://[::1]:8080", {}, factory)));
}

// ── request-target semantics ─────────────────────────────────────────────────

TEST(HttpPersistentClient, EmptyTarget_SendsRoot) {
    auto factory = std::make_shared<FakeFactory>();
    auto stream = ok_stream();
    auto* stream_ptr = stream.get();
    factory->tcp_streams.push_back(std::move(stream));

    const net::http::PersistentClient client("http://a.test", {}, factory);
    const auto resp = client.exchange("", plain_get());

    ASSERT_TRUE(resp);
    EXPECT_TRUE(stream_ptr->sent.starts_with("GET / HTTP/1.1\r\n"));
}

TEST(HttpPersistentClient, PathTarget_UsedVerbatim) {
    auto factory = std::make_shared<FakeFactory>();
    auto stream = ok_stream();
    auto* stream_ptr = stream.get();
    factory->tcp_streams.push_back(std::move(stream));

    const net::http::PersistentClient client("http://a.test", {}, factory);
    const auto resp = client.exchange("/ip?x=1", plain_get());

    ASSERT_TRUE(resp);
    EXPECT_TRUE(stream_ptr->sent.starts_with("GET /ip?x=1 HTTP/1.1\r\n"));
}

TEST(HttpPersistentClient, AbsoluteUrl_ContributesOnlyPathAndQuery) {
    auto factory = std::make_shared<FakeFactory>();
    auto stream = ok_stream();
    auto* stream_ptr = stream.get();
    factory->tcp_streams.push_back(std::move(stream));

    // Through the HttpClient port callers pass full URLs; the origin still
    // comes from the base URL — only path+query are used.
    const net::http::PersistentClient client("http://a.test", {}, factory);
    const auto resp = client.exchange("http://other.test/ip?q=1", plain_get());

    ASSERT_TRUE(resp);
    EXPECT_TRUE(stream_ptr->sent.starts_with("GET /ip?q=1 HTTP/1.1\r\n"));
    ASSERT_EQ(factory->tcp_hosts.size(), 1);
    EXPECT_EQ(factory->tcp_hosts.front(), "a.test");  // base origin, not other.test
}

// ── redirect handling ────────────────────────────────────────────────────────

TEST(HttpPersistentClient, ConnectionClose_RebuildsSession) {
    auto factory = std::make_shared<FakeFactory>();
    factory->tcp_streams.push_back(
        stream_responding("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3\r\n\r\none"));
    factory->tcp_streams.push_back(ok_stream("two"));

    const net::http::PersistentClient client("http://a.test", {}, factory);
    auto first = client.exchange("/one", plain_get());
    auto second = client.exchange("/two", plain_get());

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->text(), "one");
    EXPECT_EQ(second->text(), "two");
    EXPECT_EQ(factory->tcp_hosts.size(), 2);
}

TEST(HttpPersistentClient, Http10KeepAlive_ReusesSession) {
    auto factory = std::make_shared<FakeFactory>();
    auto stream = std::make_unique<FakeStream>();
    auto* stream_ptr = stream.get();
    stream->input = "HTTP/1.0 200 OK\r\nConnection: Keep-Alive\r\nContent-Length: 3\r\n\r\none"
                    "HTTP/1.0 200 OK\r\nConnection: Keep-Alive\r\nContent-Length: 3\r\n\r\ntwo";
    factory->tcp_streams.push_back(std::move(stream));

    net::http::Options opts{.version = net::http::HttpVersion::V1_0};
    const net::http::PersistentClient client("http://a.test", opts, factory);

    auto first = client.exchange("/one", plain_get());
    auto second = client.exchange("/two", plain_get());

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->text(), "one");
    EXPECT_EQ(second->text(), "two");
    ASSERT_EQ(factory->tcp_hosts.size(), 1);
    EXPECT_TRUE(stream_ptr->sent.starts_with("GET /one HTTP/1.0\r\n"));
    EXPECT_NE(stream_ptr->sent.find("Connection: keep-alive\r\n"), std::string::npos);
    EXPECT_NE(stream_ptr->sent.find("GET /two HTTP/1.0\r\n"), std::string::npos);
}

TEST(HttpPersistentClient, SameOriginRedirect_FollowedOnSameConnection) {
    auto factory = std::make_shared<FakeFactory>();
    auto stream = std::make_unique<FakeStream>();
    auto* stream_ptr = stream.get();
    // One stream serves both exchanges: the 302 first, then the final 200.
    // Each read returns at most one response — no pipelining.
    const auto first = "HTTP/1.1 302 Found\r\nLocation: /login\r\nContent-Length: 0\r\n\r\n";
    stream->input = std::string(first) + "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfinal";
    stream->max_chunk = std::char_traits<char>::length(first);
    factory->tcp_streams.push_back(std::move(stream));

    const net::http::PersistentClient client("http://a.test", {}, factory);
    const auto resp = client.exchange("/start", plain_get());

    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "final");
    EXPECT_EQ(factory->tcp_hosts.size(), 1);  // same connection reused

    // First request goes to the original target, follow-up to the redirect.
    ASSERT_NE(stream_ptr->sent.find("GET /start HTTP/1.1\r\n"), std::string::npos);
    ASSERT_NE(stream_ptr->sent.find("GET /login HTTP/1.1\r\n"), std::string::npos);
}

TEST(HttpPersistentClient, Redirect_RewritesMethodToGetAndDropsBody) {
    auto factory = std::make_shared<FakeFactory>();
    auto stream = std::make_unique<FakeStream>();
    auto* stream_ptr = stream.get();
    const auto redirect = "HTTP/1.1 302 Found\r\nLocation: /next\r\nContent-Length: 0\r\n\r\n";
    stream->input = std::string(redirect) + "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    stream->max_chunk = std::char_traits<char>::length(redirect);
    factory->tcp_streams.push_back(std::move(stream));

    const net::http::PersistentClient client("http://a.test", {}, factory);
    auto req = plain_get();
    req.method = Method::POST;
    req.set_body("payload");
    const auto resp = client.exchange("/start", req);

    ASSERT_TRUE(resp);
    // 301/302/303: the follow-up is a bodyless GET.
    const auto followup = stream_ptr->sent.find("GET /next HTTP/1.1\r\n");
    ASSERT_NE(followup, std::string::npos);
    EXPECT_EQ(stream_ptr->sent.find("Content-Length", followup), std::string::npos);
}

TEST(HttpPersistentClient, CrossOriginRedirect_FollowedViaTransientClient) {
    auto factory = std::make_shared<FakeFactory>();
    factory->tcp_streams.push_back(
        stream_responding("HTTP/1.1 302 Found\r\nLocation: http://b.test/moved\r\nContent-Length: 0\r\n\r\n"));
    factory->tcp_streams.push_back(ok_stream("cross"));

    const net::http::PersistentClient client("http://a.test", {}, factory);

    auto req = plain_get();
    req.headers.emplace("X-Custom", "keep-me");
    const auto resp = client.exchange("/start", req);

    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->text(), "cross");
    ASSERT_EQ(factory->tcp_hosts.size(), 2);
    EXPECT_EQ(factory->tcp_hosts[0], "a.test");
    EXPECT_EQ(factory->tcp_hosts[1], "b.test");
}

TEST(HttpPersistentClient, RedirectLimitExceeded_ReturnsError) {
    auto factory = std::make_shared<FakeFactory>();
    // The persistent connection is reused for every hop; one stream with a
    // scripted 302 chain keeps answering "redirect to /again" forever.
    constexpr auto hop = "HTTP/1.1 302 Found\r\nLocation: /again\r\nContent-Length: 0\r\n\r\n";
    auto stream = std::make_unique<FakeStream>();
    stream->input = std::string(hop) + hop + hop + hop;
    stream->max_chunk = std::char_traits<char>::length(hop);
    factory->tcp_streams.push_back(std::move(stream));

    net::http::Options opts;
    opts.max_redirects = 2;
    const net::http::PersistentClient client("http://a.test", opts, factory);
    const auto resp = client.exchange("/loop", plain_get());

    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::REDIRECT_LIMIT_EXCEEDED);
}

TEST(HttpPersistentClient, ConnectFailure_ReturnsConnectError) {
    auto factory = std::make_shared<FakeFactory>();
    auto refused = std::make_unique<FakeStream>();
    refused->connect_error = IoError::CONNECTION_FAILED;
    factory->tcp_streams.push_back(std::move(refused));

    const net::http::PersistentClient client("http://a.test", {}, factory);
    const auto resp = client.exchange("/", plain_get());

    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::CONNECT_FAILED);
}

TEST(HttpPersistentClient, HttpsBase_UsesTlsStream) {
    auto factory = std::make_shared<FakeFactory>();
    factory->tls_streams.push_back(ok_stream());

    const net::http::PersistentClient client("https://a.test", {}, factory);
    const auto resp = client.exchange("/", plain_get());

    ASSERT_TRUE(resp);
    ASSERT_EQ(factory->tls_hosts.size(), 1);
    EXPECT_TRUE(factory->tcp_hosts.empty());
}

// ── transient Client through the same factory ────────────────────────────────

TEST(HttpClient, InvalidUrl_ReturnsInvalidUrlError) {
    auto factory = std::make_shared<FakeFactory>();
    const net::http::Client client({}, factory);

    const auto resp = client.exchange("ftp://a.test/", plain_get());
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::INVALID_URL);
}

TEST(HttpClient, HttpsUrl_UsesTlsFactory) {
    auto factory = std::make_shared<FakeFactory>();
    factory->tls_streams.push_back(ok_stream());

    const net::http::Client client({}, factory);
    const auto resp = client.exchange("https://a.test/", plain_get());

    ASSERT_TRUE(resp);
    ASSERT_EQ(factory->tls_hosts.size(), 1);
    EXPECT_TRUE(factory->tcp_hosts.empty());
}
