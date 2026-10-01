//
// Unit tests for net::http::protocol::exchange
// (src/infrastructure/network/http/protocol/exchange.cpp).
//
// The protocol helpers live in an anonymous namespace, so every case is
// driven through the public exchange() entry point over a scripted
// in-memory Transport::Stream. The focus is the request-validation and
// response-parsing rejection paths (header smuggling, framing conflicts,
// 1xx/204/205 body rules) plus the chunked/trailer grammar.
// =============================================================================

#include "infrastructure/network/http/protocol/exchange.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/protocol/wire.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"
#include "support/util/cancellation_token.hpp"

using net::http::ErrorCode;
using net::http::Limits;
using net::http::Method;
using Transport::IoError;

namespace {

/// Scripted in-memory stream: hands out `input` in order, one queued item
/// per read_some() call so a test can force the reader to loop. An entry of
/// std::nullopt is an explicit EOF marker; a partially-consumed entry is
/// resumed on the next call.
class FakeStream final : public Transport::Stream {
public:
    /// Bytes returned by successive read_some() calls. std::nullopt models a
    /// peer that closed the connection (a 0-byte read).
    std::deque<std::optional<std::string>> script;
    std::optional<IoError> read_error;
    std::optional<size_t> read_error_at;  ///< fail the Nth read (0-based)
    std::optional<IoError> send_error;
    size_t reads = 0;
    std::string sent;

    void feed(std::string data) { script.push_back(std::move(data)); }
    void feed_eof() { script.push_back(std::nullopt); }

    [[nodiscard]] std::expected<void, IoError> ensure_connected(const Utils::CancellationToken&) override {
        return {};
    }

    void close() noexcept override {}

    [[nodiscard]] std::expected<size_t, IoError> read_some(std::span<std::uint8_t> buf,
                                                           const Utils::CancellationToken&) override {
        if (read_error && (!read_error_at || *read_error_at == reads)) {
            return std::unexpected(*read_error);
        }
        ++reads;
        if (script.empty()) {
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
        auto& head = script.front();
        if (!head) {
            script.pop_front();
            return 0;
        }
        const auto n = std::min(buf.size(), head->size());
        std::memcpy(buf.data(), head->data(), n);
        head->erase(0, n);
        if (head->empty()) {
            script.pop_front();
        }
        return n;
    }

    [[nodiscard]] std::expected<void, IoError> read_exact(std::span<std::uint8_t> buf,
                                                          const Utils::CancellationToken& token) override {
        auto remaining = buf;
        while (!remaining.empty()) {
            auto n = read_some(remaining, token);
            if (!n) {
                return std::unexpected(n.error());
            }
            remaining = remaining.subspan(*n);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, IoError> send_all(std::span<const std::uint8_t> data,
                                                        const Utils::CancellationToken&) override {
        if (send_error) {
            return std::unexpected(*send_error);
        }
        sent.append(reinterpret_cast<const char*>(data.data()), data.size());
        return {};
    }
};

net::http::protocol::WireRequest make_req(std::string target = "/dns-query") {
    net::http::protocol::WireRequest req{
        .method = Method::GET,
        .version = net::http::HttpVersion::V1_1,
        .target = std::move(target),
        .headers = {{"host", "example.test"}},
        .body = std::nullopt,
    };
    return req;
}

/// Value-initialized response, used as the fallback when a case expected a
/// value but the parser returned an error (the EXPECT above already failed).
net::http::protocol::RawResponse empty_response() {
    return {
        .status = 0,
        .version = net::http::HttpVersion::V1_1,
        .reusable = false,
        .keep_alive_max = std::nullopt,
        .keep_alive_timeout = std::nullopt,
        .headers = {},
        .trailers = {},
        .body = {},
    };
}

/// Runs exchange() with a never-cancelled token and no leftover bytes.
net::http::protocol::RawResponse expect_ok(FakeStream& stream, const net::http::protocol::WireRequest& req,
                                           const Limits& limits = {}) {
    Utils::CancellationToken token;
    std::string pending;
    auto result = net::http::protocol::exchange(stream, req, limits, pending, token);
    EXPECT_TRUE(result.has_value()) << (result ? "" : std::string(result.error().message));
    return result.value_or(empty_response());
}

ErrorCode expect_err(FakeStream& stream, const net::http::protocol::WireRequest& req, const Limits& limits = {}) {
    Utils::CancellationToken token;
    std::string pending;
    auto result = net::http::protocol::exchange(stream, req, limits, pending, token);
    EXPECT_FALSE(result.has_value()) << "expected an error, got a parsed response";
    return result ? ErrorCode::RESPONSE_PARSE_FAILED : result.error().code;
}

// ================================================================================
// Request validation (runs before any I/O)
// ================================================================================

TEST(Exchange, Validate_EmptyTarget_ReturnsInvalidRequest) {
    FakeStream stream;
    // No chunks queued: any read would fail, proving validation ran first.
    EXPECT_EQ(expect_err(stream, make_req("")), ErrorCode::INVALID_REQUEST);
    EXPECT_TRUE(stream.sent.empty());
}

TEST(Exchange, Validate_RelativeTarget_ReturnsInvalidRequest) {
    FakeStream stream;
    EXPECT_EQ(expect_err(stream, make_req("dns-query")), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_TargetWithControlChar_ReturnsInvalidRequest) {
    FakeStream stream;
    EXPECT_EQ(expect_err(stream, make_req("/a\tb")), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_TargetWithDel_ReturnsInvalidRequest) {
    FakeStream stream;
    EXPECT_EQ(expect_err(stream, make_req(std::string("/a\x7f", 3))), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_AsteriskTarget_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(expect_ok(stream, make_req("*")).status, 200);
}

TEST(Exchange, Validate_UpgradeHeader_ReturnsUnsupportedProtocol) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Upgrade", "websocket");
    EXPECT_EQ(expect_err(stream, req), ErrorCode::UNSUPPORTED_PROTOCOL);
}

TEST(Exchange, Validate_ConnectionUpgradeToken_ReturnsUnsupportedProtocol) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Connection", "keep-alive, Upgrade");
    EXPECT_EQ(expect_err(stream, req), ErrorCode::UNSUPPORTED_PROTOCOL);
}

TEST(Exchange, Validate_TransferEncodingHeader_ReturnsInvalidRequest) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Transfer-Encoding", "chunked");
    EXPECT_EQ(expect_err(stream, req), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_TrailerHeader_ReturnsInvalidRequest) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Trailer", "Expires");
    EXPECT_EQ(expect_err(stream, req), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_InvalidHeaderValue_ReturnsInvalidRequest) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("X-Bad", std::string("v\x01", 2));
    EXPECT_EQ(expect_err(stream, req), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_ConnectionHeaderWithInvalidTokenList_ReturnsInvalidRequest) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Connection", "close, , keep-alive");
    EXPECT_EQ(expect_err(stream, req), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_DuplicateContentLength_ReturnsInvalidRequest) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Content-Length", "0");
    req.headers.emplace("Content-Length", "0");
    EXPECT_EQ(expect_err(stream, req), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_NonNumericContentLength_ReturnsInvalidRequest) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Content-Length", "abc");
    EXPECT_EQ(expect_err(stream, req), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_ContentLengthNotMatchingBody_ReturnsInvalidRequest) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Content-Length", "5");
    req.body = "hi";
    EXPECT_EQ(expect_err(stream, req), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_DuplicateHostHeader_ReturnsInvalidRequest) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Host", "other.test");
    EXPECT_EQ(expect_err(stream, req), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_DuplicateConnectionHeader_ReturnsInvalidRequest) {
    FakeStream stream;
    auto req = make_req();
    req.headers.emplace("Connection", "close");
    req.headers.emplace("Connection", "keep-alive");
    EXPECT_EQ(expect_err(stream, req), ErrorCode::INVALID_REQUEST);
}

TEST(Exchange, Validate_MatchingContentLengthAndBody_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    auto req = make_req();
    req.method = Method::POST;
    req.headers.emplace("Content-Length", "2");
    req.body = "hi";
    const auto response = expect_ok(stream, req);
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.text(), "hi");
}

TEST(Exchange, Send_Failure_MapsIoError) {
    FakeStream stream;
    stream.send_error = IoError::TIMEOUT;
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::TIMEOUT);
}

// ================================================================================
// Response header parsing
// ================================================================================

TEST(Exchange, ParseHeaders_ConnectionClosedBeforeHeaders_ReturnsConnectionLost) {
    FakeStream stream;
    stream.feed_eof();  // 0-byte read signals EOF
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::CONNECTION_LOST);
}

TEST(Exchange, ParseHeaders_ReadFailure_MapsIoError) {
    FakeStream stream;
    stream.read_error = IoError::CANCELLED;
    stream.read_error_at = 0;
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::CANCELLED);
}

TEST(Exchange, ParseHeaders_HeadersExceedLimit_ReturnsHeadersTooLarge) {
    FakeStream stream;
    // Well-formed header lines but never the terminating blank line, so the
    // parse stays "incomplete" and the reader keeps looping into the cap.
    stream.feed("HTTP/1.1 200 OK\r\n");
    for (int i = 0; i < 32; ++i) {
        stream.feed("X-Pad: " + std::string(60, 'v') + "\r\n");
    }
    Limits limits;
    limits.max_header_bytes = 512;
    EXPECT_EQ(expect_err(stream, make_req(), limits), ErrorCode::HEADERS_TOO_LARGE);
}

TEST(Exchange, ParseHeaders_MalformedStatusLine_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("NOT-HTTP\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_UnsupportedMinorVersion_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.9 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_InvalidHeaderName_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nBad Name: v\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_InvalidHeaderValue_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nX-H: a\x01b\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_NonNumericContentLength_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: xyz\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_ConflictingDuplicateContentLength_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_InvalidConnectionTokenList_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nConnection: close,, keep-alive\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_MultipleTransferEncodingHeaders_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_NonChunkedTransferEncoding_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_ConnectionCloseAndKeepAlive_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nConnection: close, keep-alive\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_ContentLengthWithTransferEncoding_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_Http10Chunked_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, ParseHeaders_ContentLengthExceedsBodyLimit_ReturnsBodyTooLarge) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n");
    Limits limits;
    limits.max_body_bytes = 16;
    EXPECT_EQ(expect_err(stream, make_req(), limits), ErrorCode::BODY_TOO_LARGE);
}

TEST(Exchange, ParseHeaders_KeepAliveParameters_AreCaptured) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nKeep-Alive: timeout=5, max=100\r\nContent-Length: 0\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_EQ(response.keep_alive_max, 100u);
    EXPECT_EQ(response.keep_alive_timeout, 5u);
}

TEST(Exchange, ParseHeaders_KeepAliveIgnoresUnparsableParameter) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nKeep-Alive: timeout=abc, max=zz\r\nContent-Length: 0\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_FALSE(response.keep_alive_max.has_value());
    EXPECT_FALSE(response.keep_alive_timeout.has_value());
}

TEST(Exchange, ParseHeaders_HeaderValueIsTrimmed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nX-Pad:   spaced   \r\nContent-Length: 0\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    ASSERT_EQ(response.headers.count("X-Pad"), 1u);
    EXPECT_EQ(response.headers.find("X-Pad")->second, "spaced");
}

TEST(Exchange, Response_Http10_IsNotReusable) {
    FakeStream stream;
    stream.feed("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_EQ(response.version, net::http::HttpVersion::V1_0);
    EXPECT_FALSE(response.reusable);
}

TEST(Exchange, Response_Http10KeepAlive_IsReusable) {
    FakeStream stream;
    stream.feed("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_TRUE(response.reusable);
}

TEST(Exchange, Response_ConnectionClose_IsNotReusable) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    EXPECT_FALSE(expect_ok(stream, make_req()).reusable);
}

TEST(Exchange, Response_RequestConnectionClose_IsNotReusable) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    auto req = make_req();
    req.headers.emplace("Connection", "close");
    EXPECT_FALSE(expect_ok(stream, req).reusable);
}

// ================================================================================
// Fixed-length bodies
// ================================================================================

TEST(Exchange, FixedBody_BytesBeyondBody_AreLeftPending) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhiNEXT-RESPONSE");
    Utils::CancellationToken token;
    std::string pending;
    auto req = make_req();
    auto result = net::http::protocol::exchange(stream, req, Limits{}, pending, token);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text(), "hi");
    EXPECT_EQ(pending, "NEXT-RESPONSE");
}

TEST(Exchange, FixedBody_TruncatedBody_ReturnsConnectionLost) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
    stream.feed_eof();  // EOF mid-body
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::CONNECTION_LOST);
}

TEST(Exchange, FixedBody_ReadFailure_MapsIoError) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
    stream.read_error = IoError::TIMEOUT;
    stream.read_error_at = 1;
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::TIMEOUT);
}

TEST(Exchange, FixedBody_ArrivesAcrossMultipleReads) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\n");
    stream.feed("ab");
    stream.feed("cd");
    stream.feed("ef");
    EXPECT_EQ(expect_ok(stream, make_req()).text(), "abcdef");
}

// ================================================================================
// Close-delimited bodies
// ================================================================================

TEST(Exchange, UntilEof_NoFraming_ReadsUntilPeerCloses) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\n\r\npartial");
    stream.feed_eof();  // EOF terminates the body
    EXPECT_EQ(expect_ok(stream, make_req()).text(), "partial");
}

TEST(Exchange, UntilEof_ConnectionFailed_TerminatesBodySuccessfully) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\n\r\npartial");
    stream.read_error = IoError::CONNECTION_FAILED;
    stream.read_error_at = 1;
    const auto response = expect_ok(stream, make_req());
    EXPECT_EQ(response.text(), "partial");
    EXPECT_FALSE(response.reusable);
    EXPECT_EQ(stream.reads, 1u);
    EXPECT_TRUE(stream.script.empty());
}

TEST(Exchange, UntilEof_BodyReadTimeout_MapsIoError) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\n\r\npartial");
    stream.read_error = IoError::TIMEOUT;
    stream.read_error_at = 1;
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::TIMEOUT);
    EXPECT_EQ(stream.reads, 1u);
    EXPECT_TRUE(stream.script.empty());
}

TEST(Exchange, UntilEof_BodyReadCancelled_MapsIoError) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\n\r\npartial");
    stream.read_error = IoError::CANCELLED;
    stream.read_error_at = 1;
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::CANCELLED);
    EXPECT_EQ(stream.reads, 1u);
    EXPECT_TRUE(stream.script.empty());
}

TEST(Exchange, UntilEof_ExceedingBodyLimit_ReturnsBodyTooLarge) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\n\r\n");
    for (int i = 0; i < 8; ++i) {
        stream.feed(std::string(64, 'x'));
    }
    Limits limits;
    limits.max_body_bytes = 100;
    EXPECT_EQ(expect_err(stream, make_req(), limits), ErrorCode::BODY_TOO_LARGE);
}

// ================================================================================
// Chunked bodies and trailers
// ================================================================================

TEST(Exchange, Chunked_MultipleChunks_AreConcatenated) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
    EXPECT_EQ(expect_ok(stream, make_req()).text(), "Wikipedia");
}

TEST(Exchange, Chunked_UppercaseHexSize_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("A\r\n0123456789\r\n0\r\n\r\n");
    EXPECT_EQ(expect_ok(stream, make_req()).text(), "0123456789");
}

TEST(Exchange, Chunked_WithTrailers_TrailersAreCaptured) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("3\r\nabc\r\n0\r\nExpires: Wed, 21 Oct 2099 07:28:00 GMT\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_EQ(response.text(), "abc");
    ASSERT_EQ(response.trailers.count("Expires"), 1u);
}

TEST(Exchange, Chunked_EmptyTrailerSection_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("0\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_TRUE(response.text().empty());
    EXPECT_TRUE(response.trailers.empty());
}

TEST(Exchange, Chunked_TrailingTokenExtension_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;a=b\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_ok(stream, make_req()).text(), "hi");
}

TEST(Exchange, Chunked_ValuelessExtension_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;flag\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_ok(stream, make_req()).text(), "hi");
}

TEST(Exchange, Chunked_QuotedExtensionValue_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;name=\"quoted value\"\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_ok(stream, make_req()).text(), "hi");
}

TEST(Exchange, Chunked_QuotedExtensionWithEscape_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;name=\"a\\\"b\"\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_ok(stream, make_req()).text(), "hi");
}

TEST(Exchange, Chunked_ExtensionWithoutLeadingSemicolon_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2junk\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_ExtensionWithEmptyName_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;=v\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_ExtensionWithNonTokenSeparator_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    // "(" is neither a token char nor '=', so the name/value split is invalid.
    stream.feed("2;name(\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_ExtensionValueMissing_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;name=\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_ExtensionWithEmptyTokenValue_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;name=\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_UnterminatedQuotedExtension_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;name=\"unclosed\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_QuotedExtensionWithTrailingBackslash_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;name=\"abc\\");
    stream.feed_eof();  // EOF inside the chunk metadata
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::CONNECTION_LOST);
}

TEST(Exchange, Chunked_QuotedExtensionWithControlChar_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed(std::string("2;name=\"a\x01")
                 + "b\"\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_ExtensionWithGarbageAfterValue_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2;name=v junk\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_NonHexChunkSize_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("zz\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_EmptyChunkSizeLine_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed(";ext\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_BareLfInChunkSize_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_MissingCrlfAfterChunkData_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2\r\nhiXX0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_EofBeforeChunkData_ReturnsConnectionLost) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("8\r\nab");
    stream.feed_eof();  // EOF mid chunk
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::CONNECTION_LOST);
}

TEST(Exchange, Chunked_ReadFailureInsideBody_MapsIoError) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("8\r\nab");
    stream.read_error = IoError::TIMEOUT;
    stream.read_error_at = 2;
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::TIMEOUT);
    EXPECT_EQ(stream.reads, 2u);
    EXPECT_TRUE(stream.script.empty());
}

TEST(Exchange, Chunked_ExceedingBodyLimit_ReturnsBodyTooLarge) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("ff\r\n");
    for (int i = 0; i < 8; ++i) {
        stream.feed(std::string(64, 'x'));
    }
    Limits limits;
    limits.max_body_bytes = 100;
    EXPECT_EQ(expect_err(stream, make_req(), limits), ErrorCode::BODY_TOO_LARGE);
}

TEST(Exchange, Chunked_ChunkMetadataExceedsHeaderLimit_ReturnsHeadersTooLarge) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    // Chunk-size line never terminated: no CRLF, no bare LF, but it keeps
    // growing past max_header_bytes.
    for (int i = 0; i < 6; ++i) {
        stream.feed(std::string(64, 'x'));
    }
    Limits limits;
    limits.max_header_bytes = 128;
    EXPECT_EQ(expect_err(stream, make_req(), limits), ErrorCode::HEADERS_TOO_LARGE);
}

TEST(Exchange, Chunked_ForbiddenTrailerName_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("0\r\nContent-Length: 5\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_InvalidTrailerName_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("0\r\nBad Trailer: v\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_InvalidTrailerValue_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed(std::string("0\r\nX-T: a\x01b\r\n\r\n"));
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_IncompleteTrailers_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("0\r\n");
    for (int i = 0; i < 8; ++i) {
        stream.feed("X-Pad: 0123456789012345678901234567890123456789\r\n");
    }
    Limits limits;
    limits.max_header_bytes = 128;
    EXPECT_EQ(expect_err(stream, make_req(), limits), ErrorCode::HEADERS_TOO_LARGE);
}

TEST(Exchange, Chunked_BareLfInTrailers_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("0\r\nX-T: v\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Chunked_TrailerReadFailure_MapsIoError) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("0\r\n");
    stream.read_error = IoError::CANCELLED;
    stream.read_error_at = 2;
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::CANCELLED);
    EXPECT_EQ(stream.reads, 2u);
    EXPECT_TRUE(stream.script.empty());
}

TEST(Exchange, Chunked_TrailersExceedHeaderLimit_ReturnsHeadersTooLarge) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("0\r\n");
    // The terminator arrives in the same read that pushes the block past the
    // cap, so the loop exits before re-checking its own size limit.
    stream.feed(std::string(100, 'a'));
    stream.feed(std::string(30, 'b') + "\r\n\r\n");
    Limits limits;
    limits.max_header_bytes = 128;
    EXPECT_EQ(expect_err(stream, make_req(), limits), ErrorCode::HEADERS_TOO_LARGE);
}

// ================================================================================
// Bodyless statuses and interim responses
// ================================================================================

TEST(Exchange, HeadRequest_ResponseBody_IsDiscardedAndPending) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    Utils::CancellationToken token;
    std::string pending;
    auto req = make_req();
    req.method = Method::HEAD;
    auto result = net::http::protocol::exchange(stream, req, Limits{}, pending, token);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->text().empty());
    EXPECT_EQ(pending, "hello");
}

TEST(Exchange, NoContent_Response_IsReusable) {
    FakeStream stream;
    stream.feed("HTTP/1.1 204 No Content\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_EQ(response.status, 204);
    EXPECT_TRUE(response.reusable);
}

TEST(Exchange, NotModified_Response_IsReusable) {
    FakeStream stream;
    stream.feed("HTTP/1.1 304 Not Modified\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_EQ(response.status, 304);
    EXPECT_TRUE(response.reusable);
}

TEST(Exchange, SwitchingProtocols_ReturnsUnsupportedProtocol) {
    FakeStream stream;
    stream.feed("HTTP/1.1 101 Switching Protocols\r\nUpgrade: h2c\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::UNSUPPORTED_PROTOCOL);
}

TEST(Exchange, Interim100_IsConsumedAndFinalResponseParsed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const auto response = expect_ok(stream, make_req());
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.text(), "ok");
}

TEST(Exchange, InterimWithBodyFraming_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 103 Early Hints\r\nContent-Length: 5\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, InterimWithChunkedFraming_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 103 Early Hints\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Status205_ZeroContentLength_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 205 Reset Content\r\nContent-Length: 0\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_EQ(response.status, 205);
    EXPECT_TRUE(response.text().empty());
}

TEST(Exchange, Status205_NonZeroContentLength_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 205 Reset Content\r\nContent-Length: 3\r\n\r\nabc");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Status205_ChunkedEmptyBody_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    const auto response = expect_ok(stream, make_req());
    EXPECT_EQ(response.status, 205);
    EXPECT_TRUE(response.text().empty());
}

TEST(Exchange, Status205_ChunkedNonEmptyBody_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.feed("2\r\nhi\r\n0\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Status205_CloseDelimitedEmptyBody_IsAccepted) {
    FakeStream stream;
    stream.feed("HTTP/1.1 205 Reset Content\r\nConnection: close\r\n\r\n");
    stream.feed_eof();  // EOF with no body bytes
    EXPECT_EQ(expect_ok(stream, make_req()).status, 205);
}

TEST(Exchange, Status205_CloseDelimitedNonEmptyBody_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 205 Reset Content\r\nConnection: close\r\n\r\n");
    stream.feed("data");
    stream.feed_eof();  // EOF terminates, but the body is not empty
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Status205_NoFramingAtAll_ReturnsParseFailed) {
    FakeStream stream;
    stream.feed("HTTP/1.1 205 Reset Content\r\n\r\n");
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(Exchange, Status205_ChunkedBodyReadFailure_MapsIoError) {
    FakeStream stream;
    stream.feed("HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked\r\n\r\n");
    stream.read_error = IoError::TIMEOUT;
    stream.read_error_at = 1;
    EXPECT_EQ(expect_err(stream, make_req()), ErrorCode::TIMEOUT);
    EXPECT_EQ(stream.reads, 1u);
    EXPECT_TRUE(stream.script.empty());
}

// ================================================================================
// Pending-buffer reuse across exchanges
// ================================================================================

TEST(Exchange, PendingBytes_AreReusedForTheNextResponse) {
    Utils::CancellationToken token;
    auto req = make_req();

    FakeStream first;
    first.feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhiHTTP/1.1 204 No Content\r\n\r\n");
    std::string pending;
    auto r1 = net::http::protocol::exchange(first, req, Limits{}, pending, token);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->text(), "hi");
    ASSERT_FALSE(pending.empty());

    FakeStream second;
    auto r2 = net::http::protocol::exchange(second, req, Limits{}, pending, token);
    ASSERT_TRUE(r2.has_value()) << (r2 ? "" : std::string(r2.error().message));
    EXPECT_EQ(r2->status, 204);
    EXPECT_TRUE(pending.empty());
}

TEST(Exchange, OneShotOverload_ParsesResponse) {
    FakeStream stream;
    stream.feed("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    Utils::CancellationToken token;
    auto result = net::http::protocol::exchange(stream, make_req(), Limits{}, token);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->text(), "hi");
}

// ================================================================================
// map_io_error
// ================================================================================

TEST(MapIoError, Cancelled_MapsToCancelled) {
    const auto err = net::http::protocol::map_io_error(IoError::CANCELLED, "GET /x");
    EXPECT_EQ(err.code, ErrorCode::CANCELLED);
    EXPECT_NE(std::string_view(err.message).find("cancelled"), std::string_view::npos);
}

TEST(MapIoError, Timeout_MapsToTimeout) {
    const auto err = net::http::protocol::map_io_error(IoError::TIMEOUT, "GET /x");
    EXPECT_EQ(err.code, ErrorCode::TIMEOUT);
    EXPECT_NE(std::string_view(err.message).find("timed out"), std::string_view::npos);
}

TEST(MapIoError, ConnectionFailed_MapsToConnectionLost) {
    const auto err = net::http::protocol::map_io_error(IoError::CONNECTION_FAILED, "GET /x");
    EXPECT_EQ(err.code, ErrorCode::CONNECTION_LOST);
    EXPECT_NE(std::string_view(err.message).find("connection lost"), std::string_view::npos);
}

TEST(RawResponse, Accessors_ExposeBodyAsTextAndBytes) {
    const net::http::protocol::RawResponse response{
        .status = 200,
        .version = net::http::HttpVersion::V1_1,
        .reusable = false,
        .keep_alive_max = std::nullopt,
        .keep_alive_timeout = std::nullopt,
        .headers = {},
        .trailers = {},
        .body = "abc",
    };
    EXPECT_EQ(response.size(), 3u);
    EXPECT_EQ(response.bytes().size(), 3u);
    EXPECT_EQ(response.text(), "abc");
}

}  // namespace
