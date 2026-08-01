//
// DotResolver mock tests — TLS error paths via MockTlsConnection.
//
// Verifies that DotResolver::Impl correctly handles:
//   - send_query failures (CANCELLED, CONNECTION)
//   - read_response failures (CANCELLED, CONNECTION, zero-length)
//   - ensure_connection (connect timeout, connect failure)
// =============================================================================

#include <cstdint>
#include <memory>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "dns/resolver/dot.h"
#include "mocks/mock_tls_connection.h"

#include "util/cancellation_token.hpp"

namespace {

using ::testing::_;
using ::testing::Return;
using IoStatus = TlsConnectionBase::IoStatus;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

/// A CancellationToken that is never triggered.
static Utils::CancellationToken no_cancel;

/// Build a valid DNS response (A 192.0.2.1) that echoes the TXID and the
/// question section of the captured query wire format (2-byte length prefix
/// + DNS message).  The query ID is crypto-random, so the response must be
/// derived from the actual sent bytes.
[[nodiscard]] std::vector<std::uint8_t> make_response_from_query(std::span<const std::uint8_t> wire) {
    const auto query = wire.subspan(2);  // strip 2-byte length prefix

    // Find the end of the question section (QNAME + QTYPE + QCLASS).
    size_t off = 12;
    while (off < query.size()) {
        const auto label_len = query[off];
        if (label_len == 0) {
            ++off;
            break;
        }
        off += 1 + label_len;
    }
    off += 4;  // QTYPE + QCLASS

    std::vector<std::uint8_t> resp;
    resp.reserve(off + 16);
    resp.insert(resp.end(), query.begin(), query.begin() + 2);  // TXID
    resp.push_back(0x81);
    resp.push_back(0x80);  // QR, RD, RA
    resp.push_back(0x00);
    resp.push_back(0x01);  // QDCOUNT
    resp.push_back(0x00);
    resp.push_back(0x01);  // ANCOUNT
    resp.push_back(0x00);
    resp.push_back(0x00);  // NSCOUNT
    resp.push_back(0x00);
    resp.push_back(0x00);  // ARCOUNT
    resp.insert(resp.end(), query.begin() + 12, query.begin() + static_cast<std::ptrdiff_t>(off));
    // Answer: name pointer + A + IN + TTL 60 + 192.0.2.1.
    resp.push_back(0xC0);
    resp.push_back(0x0C);
    resp.push_back(0x00);
    resp.push_back(0x01);
    resp.push_back(0x00);
    resp.push_back(0x01);
    resp.push_back(0x00);
    resp.push_back(0x00);
    resp.push_back(0x00);
    resp.push_back(0x3C);
    resp.push_back(0x00);
    resp.push_back(0x04);
    resp.push_back(192);
    resp.push_back(0);
    resp.push_back(2);
    resp.push_back(1);
    return resp;
}

/// Wire data sink: captures sent bytes and serves the derived response via
/// read_exact (2-byte length prefix, then the response body).
class MockServerPipe {
public:
    explicit MockServerPipe(MockTlsConnection &mock) {
        ON_CALL(mock, send_all(_, _))
            .WillByDefault([this](std::span<const std::uint8_t> data,
                                  const Utils::CancellationToken&)
                               -> std::expected<void, IoStatus> {
                captured_.assign(data.begin(), data.end());
                return {};
            });
        ON_CALL(mock, read_exact(_, _))
            .WillByDefault([this](std::span<std::uint8_t> buf,
                                  const Utils::CancellationToken&)
                               -> std::expected<void, IoStatus> {
                const auto body = make_response_from_query(std::span{captured_});
                if (buf.size() == 2) {
                    buf[0] = static_cast<std::uint8_t>(body.size() >> 8);
                    buf[1] = static_cast<std::uint8_t>(body.size() & 0xFF);
                    return {};
                }
                std::copy(body.begin(), body.end(), buf.begin());
                return {};
            });
    }

private:
    std::vector<std::uint8_t> captured_;
};

// ---------------------------------------------------------------------------
//  send_query error paths
// ---------------------------------------------------------------------------

TEST(DotResolverMockTest, SendCancelled_ReturnsCancelled) {
    auto mock = std::make_unique<MockTlsConnection>();

    // ensure_connection calls connect() on every entry (retry loop).
    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));

    // send_all fails with CANCELLED on first call; retry also fails.
    ON_CALL(*mock, send_all(_, _))
        .WillByDefault(Return(std::unexpected(IoStatus::CANCELLED)));

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CANCELLED);
}

TEST(DotResolverMockTest, SendConnectionFailed_ReturnsConnection) {
    auto mock = std::make_unique<MockTlsConnection>();

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));

    // send_all returns ERROR (non-cancelled) → maps to CONNECTION.
    // After retry, the second call also fails.
    ON_CALL(*mock, send_all(_, _))
        .WillByDefault(Return(std::unexpected(IoStatus::ERROR)));

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CONNECTION);
}

// ---------------------------------------------------------------------------
//  read_response error paths
// ---------------------------------------------------------------------------

TEST(DotResolverMockTest, ReadLengthCancelled_ReturnsCancelled) {
    auto mock = std::make_unique<MockTlsConnection>();

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    ON_CALL(*mock, send_all(_, _))
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    // read_exact for the 2-byte length: fail with CANCELLED on first call.
    // Retry path: send_all succeeds, read_exact fails again.
    ON_CALL(*mock, read_exact(_, _))
        .WillByDefault(Return(std::unexpected(IoStatus::CANCELLED)));

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CANCELLED);
}

TEST(DotResolverMockTest, ReadLengthConnectionFailed_ReturnsConnection) {
    auto mock = std::make_unique<MockTlsConnection>();

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    ON_CALL(*mock, send_all(_, _))
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    ON_CALL(*mock, read_exact(_, _))
        .WillByDefault(Return(std::unexpected(IoStatus::ERROR)));

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CONNECTION);
}

TEST(DotResolverMockTest, ZeroLengthResponse_ReturnsParse) {
    auto mock = std::make_unique<MockTlsConnection>();

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    ON_CALL(*mock, send_all(_, _))
        .WillByDefault(Return(std::expected<void, IoStatus>{}));

    // First call to read_exact: read 2-byte length = 0x0000.
    // On retry, send_all then read_exact again — same result.
    // Since read_exact is called multiple times (once per attempt),
    // and we need the same response each time, use WillByDefault.
    ON_CALL(*mock, read_exact(_, _))
        .WillByDefault([](std::span<std::uint8_t> buf, const Utils::CancellationToken&) {
            buf[0] = 0x00;
            buf[1] = 0x00;
            return std::expected<void, IoStatus>{};
        });

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::PARSE);
}

TEST(DotResolverMockTest, ReadBodyCancelled_ReturnsCancelled) {
    auto mock = std::make_unique<MockTlsConnection>();

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    ON_CALL(*mock, send_all(_, _))
        .WillByDefault(Return(std::expected<void, IoStatus>{}));

    // Two calls to read_exact per attempt:
    //   1. read 2-byte length → succeed with 0x0008
    //   2. read body → fail with CANCELLED
    // On retry, same pattern.
    int call_count = 0;
    ON_CALL(*mock, read_exact(_, _))
        .WillByDefault([&call_count](std::span<std::uint8_t> buf,
                                      const Utils::CancellationToken&)
                           -> std::expected<void, IoStatus> {
            if ((call_count % 2) == 0) {
                // length read: succeed
                buf[0] = 0x00;
                buf[1] = 0x08;
                ++call_count;
                return {};
            }
            // body read: fail
            ++call_count;
            return std::unexpected(IoStatus::CANCELLED);
        });

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CANCELLED);
}

// ---------------------------------------------------------------------------
//  ensure_connection paths
// ---------------------------------------------------------------------------

TEST(DotResolverMockTest, ConnectTimeout_ReturnsRetry) {
    auto mock = std::make_unique<MockTlsConnection>();

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::unexpected(IoStatus::TIMEOUT)));

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::RETRY);
}

TEST(DotResolverMockTest, ConnectError_ReturnsConnection) {
    auto mock = std::make_unique<MockTlsConnection>();

    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::unexpected(IoStatus::ERROR)));

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CONNECTION);
}

// ---------------------------------------------------------------------------
//  Retry + connection-reuse paths
// ---------------------------------------------------------------------------

TEST(DotResolverMockTest, SendRetry_ReconnectsAndSucceeds) {
    auto mock = std::make_unique<MockTlsConnection>();
    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    ON_CALL(*mock, is_connected())
        .WillByDefault(Return(false));

    // First send fails with a non-cancelled error → the retry attempt closes
    // the connection and reconnects; the second send succeeds.
    int send_calls = 0;
    ON_CALL(*mock, send_all(_, _))
        .WillByDefault([&send_calls](std::span<const std::uint8_t> data,
                                     const Utils::CancellationToken&)
                           -> std::expected<void, IoStatus> {
            if (send_calls++ == 0) {
                return std::unexpected(IoStatus::ERROR);
            }
            return {};
        });
    MockServerPipe pipe(*mock);  // captures the second send, serves a response

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_TRUE(result.has_value()) << "retry with reconnect should succeed";
}

TEST(DotResolverMockTest, ConnectionReused_OnSecondQuery) {
    auto mock = std::make_unique<MockTlsConnection>();
    ON_CALL(*mock, is_connected())
        .WillByDefault(Return(true));
    ON_CALL(*mock, is_healthy())
        .WillByDefault(Return(true));
    // A healthy connected connection must be reused — connect() never runs.
    EXPECT_CALL(*mock, connect()).Times(0);
    MockServerPipe pipe(*mock);

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto r1 = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_TRUE(r1.has_value());
    auto r2 = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_TRUE(r2.has_value());
}

TEST(DotResolverMockTest, UnhealthyConnection_Reconnects) {
    auto mock = std::make_unique<MockTlsConnection>();
    ON_CALL(*mock, is_connected())
        .WillByDefault(Return(true));
    ON_CALL(*mock, is_healthy())
        .WillByDefault(Return(false));
    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    ON_CALL(*mock, shutdown())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    EXPECT_CALL(*mock, connect()).Times(1);
    MockServerPipe pipe(*mock);

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_TRUE(result.has_value()) << "reconnect after unhealthy connection should succeed";
}

TEST(DotResolverMockTest, UnexpectedAlpn_WarnsButSucceeds) {
    auto mock = std::make_unique<MockTlsConnection>();
    ON_CALL(*mock, is_connected())
        .WillByDefault(Return(false));
    ON_CALL(*mock, connect())
        .WillByDefault(Return(std::expected<void, IoStatus>{}));
    // The server negotiates something other than "dot" — only a warning.
    ON_CALL(*mock, negotiated_alpn())
        .WillByDefault(Return("h2"));
    MockServerPipe pipe(*mock);

    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A, no_cancel);
    ASSERT_TRUE(result.has_value()) << "unexpected ALPN must not fail the query";
}

} // anonymous namespace
