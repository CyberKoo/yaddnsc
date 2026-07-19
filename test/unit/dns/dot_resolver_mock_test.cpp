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

} // anonymous namespace
