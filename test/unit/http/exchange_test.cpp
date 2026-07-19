//
// Unit tests for src/http/http.cpp — exchange error handling.
//
// Uses a mock Transport::Stream that returns errors on demand,
// verifying that Http::exchange and its internal read_response
// correctly propagate transport errors to Http::Error.
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "http/http.h"
#include "http_type.h"
#include "util/cancellation_token.hpp"

#include "mocks/mock_stream.h"

#include "fmt.hpp"

namespace {

// =============================================================================
//  Helpers to build a valid HTTP response in a MockStream.
// =============================================================================

std::vector<std::uint8_t> make_ok_response(std::string_view body) {
    std::string raw = fmt::format(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: {}\r\n"
        "\r\n"
        "{}",
        body.size(), body);
    return {raw.begin(), raw.end()};
}

} // anonymous namespace

// ── send_all errors ───────────────────────────────────────────────────────

TEST(HttpExchangeTest, SendCancelled_ReturnsError) {
    MockStream stream;
    stream.set_send_error(Transport::IoError::CANCELLED);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CANCELLED);
    EXPECT_FALSE(stream.was_sent());
}

TEST(HttpExchangeTest, SendTimeout_ReturnsConnectionFailed) {
    MockStream stream;
    stream.set_send_error(Transport::IoError::TIMEOUT);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::TIMEOUT);
}

TEST(HttpExchangeTest, SendConnectionFailed_ReturnsError) {
    MockStream stream;
    stream.set_send_error(Transport::IoError::CONNECTION_FAILED);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CONNECTION_FAILED);
}

// ── read_some errors (from read_response internal loop) ──────────────────

TEST(HttpExchangeTest, ReadCancelled_ReturnsError) {
    auto data = make_ok_response("ok");
    MockStream stream;
    stream.set_read_data(data);
    stream.set_read_error(Transport::IoError::CANCELLED, 0);  // fail first read

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CANCELLED);
    // send_all should have succeeded before read.
    EXPECT_TRUE(stream.was_sent());
}

TEST(HttpExchangeTest, ReadConnectionFailed_ReturnsError) {
    auto data = make_ok_response("ok");
    MockStream stream;
    stream.set_read_data(data);
    stream.set_read_error(Transport::IoError::CONNECTION_FAILED, 0);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CONNECTION_FAILED);
}

TEST(HttpExchangeTest, ReadTimeout_ReturnsTimeout) {
    auto data = make_ok_response("ok");
    MockStream stream;
    stream.set_read_data(data);
    stream.set_read_error(Transport::IoError::TIMEOUT, 0);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::TIMEOUT);
}

// ── Successful exchange ──────────────────────────────────────────────────

TEST(HttpExchangeTest, SuccessfulExchange_ReturnsResponse) {
    auto data = make_ok_response("hello");
    MockStream stream;
    stream.set_read_data(data);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->body, std::vector<std::uint8_t>({'h', 'e', 'l', 'l', 'o'}));
    EXPECT_TRUE(stream.was_sent());
}

// ── Header parsing errors ────────────────────────────────────────────────

TEST(HttpExchangeTest, ReadHeadersTooLarge_ReturnsError) {
    // Build a response with a single huge header that exceeds 8192 bytes
    // but has no terminating \r\n\r\n, so headers are never complete.
    std::string raw = "HTTP/1.1 200 OK\r\n";
    raw += "X-Padding: ";
    raw += std::string(8192, 'A');
    raw += "\r\n";
    // No \r\n\r\n — headers never complete.

    auto data = std::vector<std::uint8_t>(raw.begin(), raw.end());
    MockStream stream;
    stream.set_read_data(data);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::HEADERS_TOO_LARGE);
}

TEST(HttpExchangeTest, ReadHeaderParseFailed_ReturnsError) {
    // A status line with an invalid Content-Length value causes
    // parse_response to throw, resulting in HEADER_PARSE_FAILED.
    std::string raw = "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n";
    auto data = std::vector<std::uint8_t>(raw.begin(), raw.end());
    MockStream stream;
    stream.set_read_data(data);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::HEADER_PARSE_FAILED);
}

// ── Transport EOF during header read ─────────────────────────────────────

TEST(HttpExchangeTest, ReadHeaderEof_ReturnsConnectionFailed) {
    // Transport returns 0 (EOF) before headers are complete.
    // read_response loop receives 0 from read_some and returns
    // CONNECTION_FAILED.
    MockStream stream;
    // No data set — read_some returns 0 immediately.

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CONNECTION_FAILED);
}
