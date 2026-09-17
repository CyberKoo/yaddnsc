//
// Unit tests for the net::http client domain (src/infrastructure/network/http/).
//
// Protocol exchange is exercised through a scripted in-memory Stream
// (no network I/O): fixed/chunked/close-delimited bodies, split reads,
// error mapping, limits; plus redirect policy and form encoding.
// =============================================================================

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "infrastructure/network/http/protocol/exchange.h"
#include "infrastructure/network/http/protocol/wire.h"
#include "infrastructure/network/http/redirect.h"
#include "infrastructure/network/transport/stream.h"

using net::http::ErrorCode;
using net::http::Limits;
using net::http::Method;
using Transport::IoError;

namespace {

/// Scripted in-memory Transport::Stream.
///
/// Feeds `input` to readers (optionally in small chunks to exercise
/// incremental parsing), records everything written, and can be told to
/// fail reads with a specific error (EOF is reported as
/// CONNECTION_FAILED, matching the real streams).
class FakeStream final : public Transport::Stream {
public:
    std::string input;
    size_t chunk_size = 0;  // 0 → serve as much as fits
    std::optional<IoError> fail_reads;
    std::optional<IoError> fail_writes;
    bool return_zero_reads = false;
    std::string sent;

    [[nodiscard]] std::expected<void, IoError> ensure_connected() override { return {}; }

    void close() noexcept override {}

    [[nodiscard]] std::expected<size_t, IoError> read_some(std::span<std::uint8_t> buf) override {
        if (fail_reads) {
            return std::unexpected(*fail_reads);
        }
        if (return_zero_reads) {
            return 0;
        }
        if (pos_ >= input.size()) {
            return std::unexpected(IoError::CONNECTION_FAILED);  // EOF
        }
        size_t n = std::min(buf.size(), input.size() - pos_);
        if (chunk_size > 0) {
            n = std::min(n, chunk_size);
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
        if (fail_writes) {
            return std::unexpected(*fail_writes);
        }
        sent.append(reinterpret_cast<const char*>(data.data()), data.size());
        return {};
    }

private:
    size_t pos_ = 0;
};

net::http::protocol::WireRequest make_get(std::string target) {
    return {.method = Method::GET, .target = std::move(target), .headers = {{"Host", "example.com"}}};
}

}  // namespace

// ── wire serialization ───────────────────────────────────────────────────────

TEST(HttpClientWire, SerializesRequestLineAndHeaders) {
    net::http::protocol::WireRequest req{
        .method = Method::POST,
        .target = "/submit?a=1",
        .headers = {{"Host", "example.com"}, {"Content-Length", "4"}},
        .body = "data",
    };
    const auto wire = net::http::protocol::serialize(req);
    // multimap orders headers by key.
    EXPECT_EQ(wire,
              "POST /submit?a=1 HTTP/1.1\r\n"
              "Content-Length: 4\r\n"
              "Host: example.com\r\n"
              "\r\n"
              "data");
}

TEST(HttpClientWire, EmptyTargetDefaultsToRoot) {
    net::http::protocol::WireRequest req{.method = Method::GET, .target = ""};
    EXPECT_TRUE(net::http::protocol::serialize(req).starts_with("GET / HTTP/1.1"));
}

TEST(HttpClientWire, SerializesHttp10Request) {
    net::http::protocol::WireRequest req{
        .method = Method::GET, .version = net::http::HttpVersion::V1_0, .target = "/legacy"};
    EXPECT_TRUE(net::http::protocol::serialize(req).starts_with("GET /legacy HTTP/1.0"));
}

// ── exchange: body framing ───────────────────────────────────────────────────

TEST(HttpClientExchange, FixedLengthBody) {
    FakeStream stream;
    stream.input = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Custom: v\r\n\r\nhello";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "hello");
    EXPECT_EQ(resp->headers.count("X-Custom"), 1);
}

TEST(HttpClientExchange, SplitReadsAcrossHeadersAndBody) {
    FakeStream stream;
    stream.chunk_size = 7;  // force many small reads
    stream.input = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->text(), "hello world");
}

TEST(HttpClientExchange, ChunkedBody) {
    FakeStream stream;
    stream.chunk_size = 5;
    stream.input =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\n\r\n";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->text(), "hello world");
}

TEST(HttpClientExchange, CloseDelimitedBody) {
    FakeStream stream;
    stream.input = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nstreamed body";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->text(), "streamed body");
    EXPECT_FALSE(resp->reusable);
}

TEST(HttpClientExchange, Http10KeepAliveWithContentLength_IsReusable) {
    FakeStream stream;
    stream.input = "HTTP/1.0 200 OK\r\nConnection: Keep-Alive\r\nContent-Length: 2\r\n\r\nok";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->version, net::http::HttpVersion::V1_0);
    EXPECT_TRUE(resp->reusable);
}

TEST(HttpClientExchange, Http10WithoutKeepAlive_IsNotReusable) {
    FakeStream stream;
    stream.input = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_FALSE(resp->reusable);
}

TEST(HttpClientExchange, ConnectionTokensAreExact) {
    FakeStream stream;
    stream.input = "HTTP/1.1 200 OK\r\nConnection: X-close, upgradeable\r\nContent-Length: 2\r\n\r\nok";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_TRUE(resp->reusable);

    FakeStream closing;
    closing.input = "HTTP/1.1 200 OK\r\nConnection: Foo, close\r\nContent-Length: 2\r\n\r\nok";
    resp = net::http::protocol::exchange(closing, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_FALSE(resp->reusable);
}

TEST(HttpClientExchange, RejectsUnsafeWireFramingAndProtocolUpgrade) {
    auto wire = make_get("/");
    wire.headers.emplace("Content-Length", "1");
    FakeStream mismatched_length;
    auto resp = net::http::protocol::exchange(mismatched_length, wire, Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::INVALID_REQUEST);

    wire = make_get("/");
    wire.headers.emplace("Upgrade", "websocket");
    FakeStream request_upgrade;
    resp = net::http::protocol::exchange(request_upgrade, wire, Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::UNSUPPORTED_PROTOCOL);

    FakeStream coding;
    coding.input = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n";
    resp = net::http::protocol::exchange(coding, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::RESPONSE_PARSE_FAILED);

    FakeStream upgrade;
    upgrade.input = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
    resp = net::http::protocol::exchange(upgrade, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::UNSUPPORTED_PROTOCOL);
}

TEST(HttpClientExchange, ResetContentAndChunkedTrailersFollowFramingRules) {
    FakeStream reset;
    reset.input = "HTTP/1.1 205 Reset Content\r\nContent-Length: 1\r\n\r\nx";
    auto resp = net::http::protocol::exchange(reset, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::RESPONSE_PARSE_FAILED);

    FakeStream chunked;
    chunked.input = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "2\r\nok\r\n0\r\nChecksum: abc\r\n\r\n";
    resp = net::http::protocol::exchange(chunked, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->text(), "ok");
    EXPECT_EQ(resp->trailers.find("Checksum")->second, "abc");

    FakeStream forbidden;
    forbidden.input = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "0\r\nContent-Length: 1\r\n\r\n";
    resp = net::http::protocol::exchange(forbidden, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::RESPONSE_PARSE_FAILED);

    FakeStream reset_chunked;
    reset_chunked.input = "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    resp = net::http::protocol::exchange(reset_chunked, make_get("/"), Limits{});
    ASSERT_TRUE(resp);
    EXPECT_TRUE(resp->text().empty());
}

TEST(HttpClientExchange, NoBodyStatusPreservesNextResponse) {
    FakeStream stream;
    stream.input = "HTTP/1.1 204 No Content\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    std::string pending;

    auto first = net::http::protocol::exchange(stream, make_get("/first"), Limits{}, pending);
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->text().empty());

    auto second = net::http::protocol::exchange(stream, make_get("/second"), Limits{}, pending);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->text(), "ok");
}

TEST(HttpClientExchange, ChunkedBodyPreservesNextResponse) {
    FakeStream stream;
    stream.input = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "2\r\nok\r\n0\r\n\r\n"
                   "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo";
    std::string pending;

    auto first = net::http::protocol::exchange(stream, make_get("/first"), Limits{}, pending);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->text(), "ok");

    auto second = net::http::protocol::exchange(stream, make_get("/second"), Limits{}, pending);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->text(), "two");
}

TEST(HttpClientExchange, HeadResponseHasNoBody) {
    FakeStream stream;
    stream.input = "HTTP/1.1 200 OK\r\n\r\n";  // no framing at all

    auto req = make_get("/");
    req.method = Method::HEAD;
    auto resp = net::http::protocol::exchange(stream, req, Limits{});
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_TRUE(resp->text().empty());
}

TEST(HttpClientExchange, RequestIsSerializedToStream) {
    FakeStream stream;
    stream.input = "HTTP/1.1 204 No Content\r\n\r\n";

    net::http::protocol::WireRequest req{
        .method = Method::PUT,
        .target = "/resource",
        .headers = {{"Host", "example.com"}},
        .body = "payload",
    };
    ASSERT_TRUE(net::http::protocol::exchange(stream, req, Limits{}));
    EXPECT_EQ(stream.sent,
              "PUT /resource HTTP/1.1\r\n"
              "Host: example.com\r\n"
              "\r\n"
              "payload");
}

// ── exchange: limits and errors ──────────────────────────────────────────────

TEST(HttpClientExchange, MalformedHeaders_Fail) {
    FakeStream stream;
    stream.input = "NOT AN HTTP RESPONSE\r\n\r\n";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(HttpClientExchange, InvalidContentLengthAndTransferEncoding_Fail) {
    for (const auto* response : {
             "HTTP/1.1 200 OK\r\nContent-Length: no\r\n\r\n",
             "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
         }) {
        FakeStream stream;
        stream.input = response;
        const auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
        ASSERT_FALSE(resp);
        EXPECT_EQ(resp.error().code, ErrorCode::RESPONSE_PARSE_FAILED);
    }
}

TEST(HttpClientExchange, ConflictingContentLength_Fails) {
    FakeStream stream;
    stream.input = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(HttpClientExchange, BodyExceedsLimit) {
    FakeStream stream;
    stream.input = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";

    const Limits limits{.max_header_bytes = 64 * 1024, .max_body_bytes = 10};
    auto resp = net::http::protocol::exchange(stream, make_get("/"), limits);
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::BODY_TOO_LARGE);
}

TEST(HttpClientExchange, HeadersExceedLimit) {
    FakeStream stream;
    std::string big(500, 'X');
    stream.input = "HTTP/1.1 200 OK\r\nX-Big: " + big + "\r\n\r\n";

    const Limits limits{.max_header_bytes = 100, .max_body_bytes = 1024};
    auto resp = net::http::protocol::exchange(stream, make_get("/"), limits);
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::HEADERS_TOO_LARGE);
}

TEST(HttpClientExchange, ChunkedErrorsAndLimits_Fail) {
    FakeStream malformed;
    malformed.input = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nnot-a-chunk\r\n";
    auto resp = net::http::protocol::exchange(malformed, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::RESPONSE_PARSE_FAILED);

    FakeStream too_large;
    too_large.input = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
    resp = net::http::protocol::exchange(too_large, make_get("/"), {.max_body_bytes = 4});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::BODY_TOO_LARGE);
}

TEST(HttpClientExchange, ZeroReadAndSendFailure_MapToConnectionErrors) {
    FakeStream zero;
    zero.return_zero_reads = true;
    auto resp = net::http::protocol::exchange(zero, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::CONNECTION_LOST);

    FakeStream send_failure;
    send_failure.fail_writes = IoError::TIMEOUT;
    resp = net::http::protocol::exchange(send_failure, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::TIMEOUT);
}

TEST(HttpClientExchange, CancelledStream_MapsToCancelled) {
    FakeStream stream;
    stream.fail_reads = IoError::CANCELLED;

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::CANCELLED);
}

TEST(HttpClientExchange, Timeout_MapsToTimeout) {
    FakeStream stream;
    stream.fail_reads = IoError::TIMEOUT;

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::TIMEOUT);
}

TEST(HttpClientExchange, ConnectionLost_MapsToConnectionLost) {
    FakeStream stream;
    stream.input = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort";

    auto resp = net::http::protocol::exchange(stream, make_get("/"), Limits{});
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::CONNECTION_LOST);
}

// ── redirect policy ──────────────────────────────────────────────────────────

namespace {

net::http::protocol::WireRequest post_request() {
    net::http::protocol::WireRequest req{
        .method = Method::POST,
        .target = "/api/v1/update",
        .headers = {{"Host", "api.example.com"},
                    {"Authorization", "Bearer secret"},
                    {"Content-Length", "7"},
                    {"Content-Type", "application/json"}},
        .body = "{\"a\":1}",
    };
    return req;
}

[[nodiscard]] Uri make_current_uri() {
    return Uri::parse("https://api.example.com/api/v1/update");
}

}  // namespace

TEST(HttpClientRedirect, NotARedirect_NotFollowed) {
    const auto eval =
        net::http::evaluate_redirect(200, {}, 0, {}, post_request(), make_current_uri());
    EXPECT_FALSE(eval.plan.has_value());
    EXPECT_FALSE(eval.limit_reached);
}

TEST(HttpClientRedirect, MissingLocation_NotFollowed) {
    const auto eval = net::http::evaluate_redirect(302, {}, 0, {}, post_request(), make_current_uri());
    EXPECT_FALSE(eval.plan.has_value());
}

TEST(HttpClientRedirect, FollowDisabled_NotFollowed) {
    net::http::Options opts;
    opts.follow_redirects = false;
    const auto eval = net::http::evaluate_redirect(302, {{"Location", "/new"}}, 0, opts, post_request(), make_current_uri());
    EXPECT_FALSE(eval.plan.has_value());
}

TEST(HttpClientRedirect, MovedPermanently_RewritesToGetDropsBody) {
    const auto eval = net::http::evaluate_redirect(301, {{"Location", "/moved"}}, 0, {}, post_request(), make_current_uri());
    ASSERT_TRUE(eval.plan.has_value());
    EXPECT_EQ(eval.plan->next.method, Method::GET);
    EXPECT_FALSE(eval.plan->next.body.has_value());
    EXPECT_EQ(eval.plan->next.target, "/moved");
    // Same origin: authorization preserved.
    EXPECT_EQ(eval.plan->next.headers.count("Authorization"), 1);
}

TEST(HttpClientRedirect, TemporaryRedirect_PreservesMethodAndBody) {
    const auto eval = net::http::evaluate_redirect(307, {{"Location", "/retry"}}, 0, {}, post_request(), make_current_uri());
    ASSERT_TRUE(eval.plan.has_value());
    EXPECT_EQ(eval.plan->next.method, Method::POST);
    ASSERT_TRUE(eval.plan->next.body.has_value());
    EXPECT_EQ(*eval.plan->next.body, "{\"a\":1}");
    EXPECT_EQ(eval.plan->next.headers.count("Content-Type"), 1);
    EXPECT_EQ(eval.plan->next.headers.count("Content-Length"), 1);
}

TEST(HttpClientRedirect, CrossOrigin_StripsAuthorization) {
    const auto eval = net::http::evaluate_redirect(
        302, {{"Location", "https://other.example.net/api"}}, 0, {}, post_request(), make_current_uri());
    ASSERT_TRUE(eval.plan.has_value());
    EXPECT_TRUE(eval.plan->cross_origin);
    EXPECT_EQ(eval.plan->host, "other.example.net");
    EXPECT_EQ(eval.plan->scheme, "https");
    EXPECT_EQ(eval.plan->port, 443);
    EXPECT_EQ(eval.plan->next.headers.count("Authorization"), 0);
    // Host header rebuilt for the target origin.
    EXPECT_EQ(eval.plan->next.headers.count("Host"), 1);
}

TEST(HttpClientRedirect, AbsoluteLocationWithPort) {
    const auto eval = net::http::evaluate_redirect(
        302, {{"Location", "http://plain.example.com:8080/x"}}, 0, {}, post_request(), make_current_uri());
    ASSERT_TRUE(eval.plan.has_value());
    EXPECT_EQ(eval.plan->scheme, "http");
    EXPECT_EQ(eval.plan->port, 8080);
    EXPECT_EQ(eval.plan->next.target, "/x");
}

TEST(HttpClientRedirect, RelativeLocation_MergesPath) {
    const auto eval = net::http::evaluate_redirect(302, {{"Location", "next"}}, 0, {}, post_request(), make_current_uri());
    ASSERT_TRUE(eval.plan.has_value());
    EXPECT_EQ(eval.plan->next.target, "/api/v1/next");
    EXPECT_FALSE(eval.plan->cross_origin);
}

TEST(HttpClientRedirect, LimitExceeded_ReportsLimit) {
    net::http::Options opts;
    opts.max_redirects = 3;
    const auto eval = net::http::evaluate_redirect(302, {{"Location", "/loop"}}, 3, opts, post_request(), make_current_uri());
    EXPECT_FALSE(eval.plan.has_value());
    EXPECT_TRUE(eval.limit_reached);
}
