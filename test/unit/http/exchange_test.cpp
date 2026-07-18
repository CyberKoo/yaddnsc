//
// Unit tests for src/http/http.cpp — exchange error handling.
//
// Uses a mock Transport::Stream that returns errors on demand,
// verifying that Http::exchange and its internal read_response
// correctly propagate transport errors to Http::Error.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "http/http.h"
#include "http_type.h"
#include "network/transport/stream.h"
#include "util/cancellation_token.hpp"

#include "fmt.hpp"

namespace {

// =============================================================================
//  MockStream — a Transport::Stream that returns configurable errors.
//
//  Supports three modes:
//    1. Normal:   read_some / send_all succeed with provided data.
//    2. SendFail: send_all returns a specified IoError.
//    3. ReadFail: read_some returns a specified IoError (after N calls).
// =============================================================================

class MockStream final : public Transport::Stream {
public:
    // ── Normal mode (data to be read) ──
    explicit MockStream(std::vector<std::uint8_t> data) noexcept
        : data_(std::move(data)) {}

    // ── Fail mode ──
    /// send_all will return error on first call.
    void set_send_error(Transport::IoError err) noexcept { send_error_ = err; }

    /// read_some will return error after @p ok_calls successful reads.
    void set_read_error(Transport::IoError err, int ok_calls = 0) noexcept {
        read_error_ = err;
        read_ok_before_fail_ = ok_calls;
    }

    [[nodiscard]] std::expected<size_t, Transport::IoError> read_some(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken & /*cancel_token*/) override {
        if (read_ok_before_fail_ > 0) {
            --read_ok_before_fail_;
        } else if (read_error_.has_value()) {
            return std::unexpected(*read_error_);
        }
        const size_t avail = data_.size() - pos_;
        if (avail == 0) return size_t{0};
        const size_t take = std::min(buf.size(), avail);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(pos_), take, buf.begin());
        pos_ += take;
        return take;
    }

    [[nodiscard]] std::expected<void, Transport::IoError> read_exact(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken & /*cancel_token*/) override {
        const size_t avail = data_.size() - pos_;
        if (avail < buf.size()) {
            return std::unexpected(Transport::IoError::CONNECTION_FAILED);
        }
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(pos_), buf.size(), buf.begin());
        pos_ += buf.size();
        return {};
    }

    [[nodiscard]] std::expected<void, Transport::IoError> send_all(
        std::span<const std::uint8_t> /*data*/,
        const Utils::CancellationToken & /*cancel_token*/) override {
        if (send_error_.has_value()) {
            return std::unexpected(*send_error_);
        }
        sent_ = true;
        return {};
    }

    bool was_sent() const noexcept { return sent_; }

private:
    std::vector<std::uint8_t> data_;
    size_t pos_ = 0;
    bool sent_ = false;

    std::optional<Transport::IoError> send_error_;
    std::optional<Transport::IoError> read_error_;
    int read_ok_before_fail_ = 0;
};

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
    MockStream stream(std::vector<std::uint8_t>{});
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
    MockStream stream(std::vector<std::uint8_t>{});
    stream.set_send_error(Transport::IoError::TIMEOUT);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::TIMEOUT);
}

TEST(HttpExchangeTest, SendConnectionFailed_ReturnsError) {
    MockStream stream(std::vector<std::uint8_t>{});
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
    MockStream stream(data);
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
    MockStream stream(data);
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
    MockStream stream(data);
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
    MockStream stream(data);

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
    MockStream stream(data);

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
    MockStream stream(data);

    HttpRequest req;
    req.method = HttpMethod::GET;

    Utils::CancellationToken cancel;
    auto result = Http::exchange(stream, "/", req, "host", "agent", cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::HEADER_PARSE_FAILED);
}
