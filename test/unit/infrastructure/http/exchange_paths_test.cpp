//
// Unit tests for the wire serializer and the failure paths of the HTTP exchange
// (src/infrastructure/http/protocol/wire.cpp and exchange.cpp).
//
// Everything runs against a scripted in-memory stream (the net::Stream
// injection point): the request guard that refuses an unsafe request before a
// byte is written, the response-header parser's smuggling defences, the strict
// RFC 9112 chunk-size / trailer grammar, and the close-delimited body.
//
// The happy paths (framing, 1xx, keep-alive carry-over, the read window and the
// redirect policy) live in http_coro_test.cpp.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; the coroutine in run_exchange() is therefore assertion-free.
//

#include <algorithm>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <expected>
#include <gtest/gtest.h>

#include "coro/coro.h"
#include "infrastructure/http/error.h"
#include "infrastructure/http/protocol/exchange.h"
#include "infrastructure/http/protocol/wire.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"

namespace {

using http::Error;
using http::ErrorCode;

/// A request header as (name, value), so a table of header cases stays readable.
using Header = std::pair<std::string, std::string>;

/// The shortest bodyless response, for cases where only the request guard matters.
constexpr std::string_view NO_CONTENT = "HTTP/1.1 204 No Content\r\n\r\n";

/// A chunked response whose chunk-size line is `size_line`, carrying the chunk "hello".
[[nodiscard]] std::string chunked(const std::string_view size_line) {
    return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + std::string(size_line) + "\r\nhello\r\n0\r\n\r\n";
}

/// A chunked response that ends at once, with `trailers` as its whole trailer section.
[[nodiscard]] std::string chunked_trailed(const std::string_view trailers) {
    return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n" + std::string(trailers);
}

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

/// Run one exchange against a scripted stream.
[[nodiscard]] std::expected<http::protocol::RawResponse, Error> run_exchange(const std::string_view response_wire,
                                                                             const http::protocol::WireRequest& request,
                                                                             const http::Limits& limits = {}) {
    ScriptedStream stream{std::string(response_wire)};
    std::string pending;
    return coro::run([&]() -> coro::Task<std::expected<http::protocol::RawResponse, Error>> {
        co_return co_await http::protocol::exchange(stream, request, limits, pending);
    }());
}

[[nodiscard]] http::protocol::WireRequest get_to(std::string target = "/dns-query") {
    http::protocol::WireRequest request;
    request.method = http::Method::GET;
    request.target = std::move(target);
    request.headers.emplace("Host", "example.test");
    return request;
}

[[nodiscard]] http::protocol::WireRequest get_with(const Header& header) {
    auto request = get_to();
    request.headers.emplace(header.first, header.second);
    return request;
}

/// Expect the response parser to reject `response_wire` with `code` and `message`.
void expect_response_rejected(const std::string_view response_wire, const ErrorCode code,
                              const std::string_view message, const http::Limits& limits = {}) {
    const auto response = run_exchange(response_wire, get_to(), limits);
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, code);
    EXPECT_EQ(response.error().message, message);
}

/// Expect the request guard to reject `request` before a byte reaches the stream.
void expect_request_rejected(const http::protocol::WireRequest& request, const ErrorCode code,
                             const std::string_view message) {
    const auto response = run_exchange(NO_CONTENT, request);
    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, code);
    EXPECT_EQ(response.error().message, message);
}

// ---------------------------------------------------------------------------
// wire serialization
// ---------------------------------------------------------------------------

TEST(HttpWireSerialize, methodNamesMatchTheRequestLine) {
    EXPECT_EQ(http::protocol::method_name(http::Method::GET), "GET");
    EXPECT_EQ(http::protocol::method_name(http::Method::POST), "POST");
    EXPECT_EQ(http::protocol::method_name(http::Method::PUT), "PUT");
    EXPECT_EQ(http::protocol::method_name(http::Method::DEL), "DELETE");
    EXPECT_EQ(http::protocol::method_name(http::Method::PATCH), "PATCH");
    EXPECT_EQ(http::protocol::method_name(http::Method::HEAD), "HEAD");
    EXPECT_EQ(http::protocol::method_name(http::Method::OPTIONS), "OPTIONS");
}

TEST(HttpWireSerialize, emptyTargetFallsBackToTheRootPath) {
    http::protocol::WireRequest request;
    request.method = http::Method::HEAD;
    request.version = http::HttpVersion::V1_0;
    request.headers.emplace("Connection", "close");

    EXPECT_EQ(http::protocol::serialize(request), "HEAD / HTTP/1.0\r\nConnection: close\r\n\r\n");
}

TEST(HttpWireSerialize, bodyFollowsTheTerminatedHeaderBlock) {
    http::protocol::WireRequest request;
    request.method = http::Method::POST;
    request.target = "/dns-query";
    request.headers.emplace("Content-Length", "7");
    request.body = "payload";

    EXPECT_EQ(http::protocol::serialize(request), "POST /dns-query HTTP/1.1\r\nContent-Length: 7\r\n\r\npayload");
}

// ---------------------------------------------------------------------------
// request guard: the request target
// ---------------------------------------------------------------------------

TEST(HttpExchangeRequestTarget, asteriskFormIsAccepted) {
    const auto response = run_exchange(NO_CONTENT, get_to("*"));

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 204);
}

TEST(HttpExchangeRequestTarget, originFormIsRequired) {
    for (const auto& target : {"", "dns-query", "/dns query"}) {
        SCOPED_TRACE(target);
        expect_request_rejected(get_to(target), ErrorCode::INVALID_REQUEST, "invalid HTTP request target");
    }
}

// ---------------------------------------------------------------------------
// request guard: header fields
// ---------------------------------------------------------------------------

TEST(HttpExchangeRequestHeader, namesMustBeTokensAndValuesPrintable) {
    for (const auto& header : {Header{"Invalid Header", "value"}, Header{"X-Trace", "value\r\nInjected: yes"}}) {
        SCOPED_TRACE(header.first);
        expect_request_rejected(get_with(header), ErrorCode::INVALID_REQUEST, "invalid HTTP request header");
    }
}

TEST(HttpExchangeRequestHeader, connectionMustBeATokenList) {
    expect_request_rejected(get_with({"Connection", "clo se"}), ErrorCode::INVALID_REQUEST,
                            "invalid HTTP request header");
}

TEST(HttpExchangeRequestHeader, upgradeIsRejectedInEitherForm) {
    // A direct Upgrade field and the Connection-token spelling must both fail:
    // this client speaks HTTP/1.x framing only.
    for (const auto& header : {Header{"Upgrade", "websocket"}, Header{"Connection", "upgrade"}}) {
        SCOPED_TRACE(header.first);
        expect_request_rejected(get_with(header), ErrorCode::UNSUPPORTED_PROTOCOL,
                                "HTTP protocol upgrade is not supported");
    }
}

TEST(HttpExchangeRequestHeader, transferCodingAndTrailersAreRejected) {
    // Either field would let the caller re-frame a body the client already
    // framed, which is a request-smuggling vector.
    for (const auto& header : {Header{"Transfer-Encoding", "chunked"}, Header{"Trailer", "X-Checksum"}}) {
        SCOPED_TRACE(header.first);
        expect_request_rejected(get_with(header), ErrorCode::INVALID_REQUEST,
                                "request transfer coding and trailers are not supported");
    }
}

TEST(HttpExchangeRequestHeader, contentLengthMustBeNumeric) {
    for (const auto& value : {"abc", "", "1 2"}) {
        SCOPED_TRACE(value);
        expect_request_rejected(get_with({"Content-Length", value}), ErrorCode::INVALID_REQUEST,
                                "invalid HTTP Content-Length");
    }
}

TEST(HttpExchangeRequestHeader, duplicateRoutingOrFramingHeadersConflict) {
    // Two Host fields, two Connection fields and two Content-Length fields each
    // leave the peer's routing and framing ambiguous.
    expect_request_rejected(get_with({"Host", "other.test"}), ErrorCode::INVALID_REQUEST,
                            "conflicting HTTP framing or routing headers");

    auto connections = get_to();
    connections.headers.emplace("Connection", "close");
    connections.headers.emplace("Connection", "keep-alive");
    expect_request_rejected(connections, ErrorCode::INVALID_REQUEST, "conflicting HTTP framing or routing headers");

    auto lengths = get_to();
    lengths.headers.emplace("Content-Length", "0");
    lengths.headers.emplace("Content-Length", "0");
    expect_request_rejected(lengths, ErrorCode::INVALID_REQUEST, "conflicting HTTP framing or routing headers");
}

TEST(HttpExchangeRequestHeader, contentLengthMustMatchTheBodyActuallySent) {
    expect_request_rejected(get_with({"Content-Length", "5"}), ErrorCode::INVALID_REQUEST,
                            "conflicting HTTP framing or routing headers");

    auto agreed = get_with({"Content-Length", "5"});
    agreed.body = "hello";
    const auto response = run_exchange(NO_CONTENT, agreed);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 204);
}

TEST(HttpExchangeRequestHeader, connectionCloseEndsTheSessionAfterOneExchange) {
    const auto response =
        run_exchange("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", get_with({"Connection", "close"}));

    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(response->reusable);
}

// ---------------------------------------------------------------------------
// response header parser: the smuggling defences
// ---------------------------------------------------------------------------

TEST(HttpExchangeResponseHeader, malformedStatusLineIsRejected) {
    expect_response_rejected("NOT HTTP AT ALL\r\n\r\n", ErrorCode::RESPONSE_PARSE_FAILED, "malformed response headers");
}

TEST(HttpExchangeResponseHeader, unknownMinorVersionIsRejected) {
    for (const auto& version : {"HTTP/1.2 200 OK\r\n\r\n", "HTTP/1.9 200 OK\r\n\r\n"}) {
        SCOPED_TRACE(version);
        expect_response_rejected(version, ErrorCode::RESPONSE_PARSE_FAILED, "unsupported HTTP version");
    }
}

TEST(HttpExchangeResponseHeader, contentLengthMustBeNumeric) {
    expect_response_rejected("HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\n", ErrorCode::RESPONSE_PARSE_FAILED,
                             "invalid Content-Length header");
}

TEST(HttpExchangeResponseHeader, conflictingDuplicateContentLengthIsRejected) {
    // Two Content-Length fields that disagree are the classic desync vector.
    expect_response_rejected("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nab",
                             ErrorCode::RESPONSE_PARSE_FAILED, "conflicting Content-Length headers");
}

TEST(HttpExchangeResponseHeader, contentLengthOverTheLimitIsRejected) {
    http::Limits limits;
    limits.max_body_bytes = 2;
    expect_response_rejected("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", ErrorCode::BODY_TOO_LARGE,
                             "Content-Length exceeds limit", limits);
}

TEST(HttpExchangeResponseHeader, connectionMustBeATokenList) {
    // A trailing comma leaves an empty element, so the list is not well formed.
    for (const auto& value : {"close,", "clo se"}) {
        SCOPED_TRACE(value);
        const std::string wire =
            std::string("HTTP/1.1 200 OK\r\nConnection: ") + value + "\r\nContent-Length: 0\r\n\r\n";
        expect_response_rejected(wire, ErrorCode::RESPONSE_PARSE_FAILED, "invalid Connection header");
    }
}

TEST(HttpExchangeResponseHeader, connectionMustNotNameCloseAndKeepAliveTogether) {
    expect_response_rejected("HTTP/1.1 200 OK\r\nConnection: close, keep-alive\r\nContent-Length: 0\r\n\r\n",
                             ErrorCode::RESPONSE_PARSE_FAILED, "conflicting Connection directives");
}

TEST(HttpExchangeResponseHeader, onlyTheChunkedTransferCodingIsAccepted) {
    // Accepting `gzip, chunked` without decoding gzip would corrupt the
    // representation, so any coding other than a bare `chunked` is refused.
    expect_response_rejected("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", ErrorCode::RESPONSE_PARSE_FAILED,
                             "unsupported Transfer-Encoding");
    expect_response_rejected(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        ErrorCode::RESPONSE_PARSE_FAILED, "multiple Transfer-Encoding headers");
}

TEST(HttpExchangeResponseHeader, http10CannotUseChunkedEncoding) {
    // HTTP/1.0 has no chunked coding, so a peer claiming both is lying or broken.
    expect_response_rejected("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                             ErrorCode::RESPONSE_PARSE_FAILED, "HTTP/1.0 response uses chunked encoding");
}

TEST(HttpExchangeResponseHeader, keepAliveParametersAreCarried) {
    const auto response =
        run_exchange("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nKeep-Alive: max=42, timeout=7\r\n\r\n", get_to());

    ASSERT_TRUE(response.has_value());
    ASSERT_TRUE(response->keep_alive_max.has_value());
    ASSERT_TRUE(response->keep_alive_timeout.has_value());
    EXPECT_EQ(*response->keep_alive_max, 42U);
    EXPECT_EQ(*response->keep_alive_timeout, 7U);
    EXPECT_TRUE(response->reusable);
}

TEST(HttpExchangeResponseBody, closeDelimitedRunsToThePeerClose) {
    // Neither Content-Length nor chunked: the body ends where the peer stops
    // writing, and such a connection cannot be reused.
    const auto response = run_exchange("HTTP/1.1 200 OK\r\n\r\nclose delimited", get_to());

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->text(), "close delimited");
    EXPECT_FALSE(response->reusable);
}

TEST(HttpExchangeResponseBody, oneShotOverloadCarriesNothingForward) {
    // The overload without a `pending` is for callers that own the whole
    // connection, so it runs the same exchange and keeps nothing afterwards.
    ScriptedStream stream{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokleftover"};
    const auto response = coro::run(http::protocol::exchange(stream, get_to(), http::Limits{}));

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->text(), "ok");
    EXPECT_TRUE(response->reusable);
}

// ---------------------------------------------------------------------------
// chunk coding: the RFC 9112 size line
// ---------------------------------------------------------------------------

TEST(HttpExchangeChunkCoding, sizeLineAcceptsWellFormedExtensions) {
    for (const auto& size_line :
         {"5", "5;ext", "5;a;b=c", "5;ext=value", "5;ext=\"v\"", "5;e=\"a\\\"b\"", "5;ext=\"v\";other=1"}) {
        SCOPED_TRACE(size_line);
        const auto response = run_exchange(chunked(size_line), get_to());
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->text(), "hello");
    }
}

TEST(HttpExchangeChunkCoding, sizeLineRejectsMalformedExtensions) {
    for (const auto& size_line : {
             "5;=v",       // empty extension name
             "5;a b",      // name not followed by a value
             "5;a=",       // value missing entirely
             "5;a=,b",     // value starts with a non-token byte
             "5;e=\"a\\",  // escape character at the end of the quoted string
             "5;e=\"abc",  // quoted string never closed
             "5;e=\"a\"x"  // trailing byte after the quoted value
         }) {
        SCOPED_TRACE(size_line);
        expect_response_rejected(chunked(size_line), ErrorCode::RESPONSE_PARSE_FAILED,
                                 "malformed chunk size or extension");
    }
}

TEST(HttpExchangeChunkCoding, sizeLineRejectsControlCharactersInsideQuotedValues) {
    expect_response_rejected(chunked("5;e=\"a\x01\""), ErrorCode::RESPONSE_PARSE_FAILED,
                             "malformed chunk size or extension");
}

TEST(HttpExchangeChunkCoding, sizeLineMustCarryAHexSize) {
    expect_response_rejected(chunked(""), ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunk size or extension");
    expect_response_rejected(chunked("zz"), ErrorCode::RESPONSE_PARSE_FAILED, "invalid chunk size");
}

TEST(HttpExchangeChunkCoding, delimiterMustBeCrLf) {
    // A bare LF in the size line is a smuggling trick: a proxy and an origin that
    // disagree about where the line ends see two different messages.
    expect_response_rejected("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\nhello",
                             ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunk delimiter");
}

TEST(HttpExchangeChunkCoding, trailerSectionMustParse) {
    for (const auto& trailers : {"X T: 1\r\n\r\n", "X-T\r\n\r\n"}) {
        SCOPED_TRACE(trailers);
        expect_response_rejected(chunked_trailed(trailers), ErrorCode::RESPONSE_PARSE_FAILED,
                                 "malformed chunked trailers");
    }
}

TEST(HttpExchangeChunkCoding, trailerSectionForbidsFramingAndRoutingFields) {
    // A trailer that re-states framing or routing is how a second reading of the
    // message gets smuggled past a proxy.
    for (const auto& trailers : {"Host: other.test\r\n\r\n", "Content-Length: 5\r\n\r\n",
                                 "Transfer-Encoding: chunked\r\n\r\n", "Connection: close\r\n\r\n"}) {
        SCOPED_TRACE(trailers);
        expect_response_rejected(chunked_trailed(trailers), ErrorCode::RESPONSE_PARSE_FAILED,
                                 "malformed, forbidden, or unsafe chunked trailer");
    }
}

}  // namespace
