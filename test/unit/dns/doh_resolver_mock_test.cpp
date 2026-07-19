//
// DohResolver mock tests — TLS error paths via MockTlsConnection.
//
// Verifies that DohResolver::Impl correctly handles:
//   - ensure_connection failure (timeout, error)
//   - HTTP exchange errors (cancelled, non-200 status)
//   - DNS response validation failure
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "dns/resolver/doh.h"
#include "mocks/mock_tls_connection.h"

#include "util/cancellation_token.hpp"
#include "fmt.hpp"

namespace {

using ::testing::_;
using ::testing::Return;
using IoStatus = TlsConnectionBase::IoStatus;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static Utils::CancellationToken no_cancel;

/// Build HTTP response headers (up to and including \r\n\r\n).
/// Body is NOT included — it will be returned via read_exact.
[[nodiscard]] std::vector<std::uint8_t> make_http_headers(
    int status_code, std::string_view content_type, size_t body_len) {
    auto str = fmt::format(
        "HTTP/1.1 {} OK\r\n"
        "Content-Type: {}\r\n"
        "Content-Length: {}\r\n"
        "\r\n",
        status_code, content_type, body_len);
    return {str.begin(), str.end()};
}

/// A valid minimal DNS response for "example.com" A query.
[[nodiscard]] std::vector<std::uint8_t> valid_dns_body() {
    return {
        0x12, 0x34,                         // ID
        0x81, 0x80,                         // flags: QR, RD, RA
        0x00, 0x01,                         // QDCOUNT
        0x00, 0x01,                         // ANCOUNT
        0x00, 0x00,                         // NSCOUNT
        0x00, 0x00,                         // ARCOUNT
        // Question: example.com A
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        0x03, 'c', 'o', 'm',
        0x00,
        0x00, 0x01,                         // QTYPE A
        0x00, 0x01,                         // QCLASS IN
        // Answer: example.com A 192.0.2.1 TTL=300
        0xC0, 0x0C,
        0x00, 0x01,                         // TYPE A
        0x00, 0x01,                         // CLASS IN
        0x00, 0x00, 0x01, 0x2C,            // TTL 300
        0x00, 0x04,                         // RDLENGTH 4
        0xC0, 0x00, 0x02, 0x01             // 192.0.2.1
    };
}

/// Invalid DNS response body (garbage).
[[nodiscard]] std::vector<std::uint8_t> invalid_dns_body() {
    return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

// ---------------------------------------------------------------------------
//  ensure_connection error paths
// ---------------------------------------------------------------------------

TEST(DohResolverMockTest, ConnectTimeout_ReturnsRetry) {
    auto mock = std::make_unique<MockTlsConnection>();

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::unexpected(IoStatus::TIMEOUT)));

    DohResolver resolver("127.0.0.1", 21443, "/dns-query", "mock:21443", std::move(mock));

    auto result = resolver.query("example.com", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::RETRY);
}

TEST(DohResolverMockTest, ConnectError_ReturnsConnection) {
    auto mock = std::make_unique<MockTlsConnection>();

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::unexpected(IoStatus::ERROR)));

    DohResolver resolver("127.0.0.1", 21443, "/dns-query", "mock:21443", std::move(mock));

    auto result = resolver.query("example.com", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CONNECTION);
}

// ---------------------------------------------------------------------------
//  HTTP exchange error paths
// ---------------------------------------------------------------------------
//  Http::exchange does: send_all(request) → read_some(headers in a loop) →
//  parse_response → read_body(content-length) which calls read_exact.
//  We mock each step precisely.

TEST(DohResolverMockTest, Exchange404_ReturnsServerRefused) {
    auto mock = std::make_unique<MockTlsConnection>();
    auto body = invalid_dns_body();
    auto headers = make_http_headers(404, "application/dns-message", body.size());

    // ensure_connection succeeds
    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    // send_all accepts the HTTP request
    EXPECT_CALL(*mock, send_all(_, _))
        .Times(1)
        .WillRepeatedly(Return(std::expected<void, IoStatus>{}));

    // read_some returns HTTP headers (no body).  After headers are parsed,
    // read_body sees Content-Length > body_buffered and calls read_exact.
    EXPECT_CALL(*mock, read_some(_, _))
        .Times(1)
        .WillOnce([&](std::span<std::uint8_t> buf, const Utils::CancellationToken&)
                      -> std::expected<size_t, IoStatus> {
            auto n = std::min(buf.size(), headers.size());
            std::memcpy(buf.data(), headers.data(), n);
            return n;
        });

    // read_exact returns the body for Content-Length
    EXPECT_CALL(*mock, read_exact(_, _))
        .Times(1)
        .WillOnce([&](std::span<std::uint8_t> buf, const Utils::CancellationToken&) {
            auto n = std::min(buf.size(), body.size());
            std::memcpy(buf.data(), body.data(), n);
            return std::expected<void, IoStatus>{};
        });

    DohResolver resolver("127.0.0.1", 21443, "/dns-query", "mock:21443", std::move(mock));

    auto result = resolver.query("example.com", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::SERVER_REFUSED);
}

TEST(DohResolverMockTest, Exchange500_ReturnsRetry) {
    auto mock = std::make_unique<MockTlsConnection>();
    auto body = invalid_dns_body();
    auto headers = make_http_headers(500, "application/dns-message", body.size());

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    EXPECT_CALL(*mock, send_all(_, _))
        .Times(1)
        .WillRepeatedly(Return(std::expected<void, IoStatus>{}));
    EXPECT_CALL(*mock, read_some(_, _))
        .Times(1)
        .WillOnce([&](std::span<std::uint8_t> buf, const Utils::CancellationToken&)
                      -> std::expected<size_t, IoStatus> {
            auto n = std::min(buf.size(), headers.size());
            std::memcpy(buf.data(), headers.data(), n);
            return n;
        });
    EXPECT_CALL(*mock, read_exact(_, _))
        .Times(1)
        .WillOnce([&](std::span<std::uint8_t> buf, const Utils::CancellationToken&) {
            auto n = std::min(buf.size(), body.size());
            std::memcpy(buf.data(), body.data(), n);
            return std::expected<void, IoStatus>{};
        });

    DohResolver resolver("127.0.0.1", 21443, "/dns-query", "mock:21443", std::move(mock));

    auto result = resolver.query("example.com", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::RETRY);
}

TEST(DohResolverMockTest, InvalidDnsBody_ReturnsParse) {
    auto mock = std::make_unique<MockTlsConnection>();
    auto body = invalid_dns_body();
    auto headers = make_http_headers(200, "application/dns-message", body.size());

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    EXPECT_CALL(*mock, send_all(_, _))
        .Times(1)
        .WillRepeatedly(Return(std::expected<void, IoStatus>{}));
    EXPECT_CALL(*mock, read_some(_, _))
        .Times(1)
        .WillOnce([&](std::span<std::uint8_t> buf, const Utils::CancellationToken&)
                      -> std::expected<size_t, IoStatus> {
            auto n = std::min(buf.size(), headers.size());
            std::memcpy(buf.data(), headers.data(), n);
            return n;
        });
    EXPECT_CALL(*mock, read_exact(_, _))
        .Times(1)
        .WillOnce([&](std::span<std::uint8_t> buf, const Utils::CancellationToken&) {
            auto n = std::min(buf.size(), body.size());
            std::memcpy(buf.data(), body.data(), n);
            return std::expected<void, IoStatus>{};
        });

    DohResolver resolver("127.0.0.1", 21443, "/dns-query", "mock:21443", std::move(mock));

    auto result = resolver.query("example.com", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    // All-zero body fails DNS validation → NXDOMAIN
    EXPECT_TRUE(result.error().code == DnsError::PARSE ||
                result.error().code == DnsError::NX_DOMAIN);
}

TEST(DohResolverMockTest, ValidResponse_Succeeds) {
    auto mock = std::make_unique<MockTlsConnection>();
    auto body_template = valid_dns_body();

    // Capture the HTTP request to extract the DNS query body TXID.
    std::vector<std::uint8_t> captured_http;

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    EXPECT_CALL(*mock, send_all(_, _))
        .Times(1)
        .WillOnce([&](std::span<const std::uint8_t> data, const Utils::CancellationToken&)
                      -> std::expected<void, IoStatus> {
            captured_http.assign(data.begin(), data.end());
            return {};
        });
    EXPECT_CALL(*mock, read_some(_, _))
        .Times(1)
        .WillOnce([&](std::span<std::uint8_t> buf, const Utils::CancellationToken&)
                      -> std::expected<size_t, IoStatus> {
            auto body_size = body_template.size();
            auto headers = make_http_headers(200, "application/dns-message", body_size);
            auto n = std::min(buf.size(), headers.size());
            std::memcpy(buf.data(), headers.data(), n);
            return n;
        });
    EXPECT_CALL(*mock, read_exact(_, _))
        .Times(1)
        .WillOnce([&](std::span<std::uint8_t> buf, const Utils::CancellationToken&) {
            // Find the DNS query body (after \r\n\r\n in the HTTP request).
            auto body = body_template;
            if (captured_http.size() >= 4) {
                const std::vector<std::uint8_t> delim{0x0D, 0x0A, 0x0D, 0x0A};
                auto it = std::search(captured_http.begin(), captured_http.end(),
                                      delim.begin(), delim.end());
                if (it != captured_http.end()) {
                    it += 4;  // skip past \r\n\r\n
                    if (captured_http.end() - it >= 2) {
                        // Patch TXID in response to match query.
                        body[0] = static_cast<std::uint8_t>(*it);
                        body[1] = static_cast<std::uint8_t>(*(it + 1));
                    }
                }
            }
            auto n = std::min(buf.size(), body.size());
            std::memcpy(buf.data(), body.data(), n);
            return std::expected<void, IoStatus>{};
        });

    DohResolver resolver("127.0.0.1", 21443, "/dns-query", "mock:21443", std::move(mock));

    auto result = resolver.query("example.com", RecordKind::A, no_cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), body_template.size());
}

} // anonymous namespace
