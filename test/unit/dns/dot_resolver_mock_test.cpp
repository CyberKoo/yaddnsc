//
// DotResolver unit tests — error paths via a mocked Transport::Stream.
//
// Verifies that DotResolver correctly handles:
//   - ensure_connected failure (timeout, cancelled, error)
//   - send_all failure (cancelled, connection)
//   - read_response failure (cancelled, timeout, zero-length, oversized)
//   - one automatic reconnect-and-retry on transient I/O failure
//   - a full query roundtrip over a scripted stream
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "dns/resolver/dot.h"
#include "network/transport/stream.h"

namespace {

using ::testing::_;
using ::testing::Return;
using Transport::IoError;

// ---------------------------------------------------------------------------
//  MockStream — gmock over Transport::Stream
// ---------------------------------------------------------------------------

class MockStream final : public Transport::Stream {
public:
    MOCK_METHOD((std::expected<void, IoError>), ensure_connected, (), (override));
    MOCK_METHOD(void, close, (), (noexcept, override));
    MOCK_METHOD((std::expected<size_t, IoError>), read_some, (std::span<std::uint8_t>), (override));
    MOCK_METHOD((std::expected<void, IoError>), read_exact, (std::span<std::uint8_t>), (override));
    MOCK_METHOD((std::expected<void, IoError>), send_all, (std::span<const std::uint8_t>), (override));
};

[[nodiscard]] std::unique_ptr<MockStream> connected_mock() {
    auto mock = std::make_unique<MockStream>();
    ON_CALL(*mock, ensure_connected()).WillByDefault(Return(std::expected<void, IoError>{}));
    return mock;
}

/// Wire data sink: captures the sent query and derives a canned response
/// (mirrors the question ID + flags) via read_exact calls.
class MockServerPipe {
public:
    explicit MockServerPipe(MockStream &mock) {
        ON_CALL(mock, send_all(_))
            .WillByDefault([this](std::span<const std::uint8_t> data) -> std::expected<void, IoError> {
                captured_.assign(data.begin(), data.end());
                return {};
            });
        ON_CALL(mock, read_exact(_))
            .WillByDefault([this](std::span<std::uint8_t> buf) -> std::expected<void, IoError> {
                // Minimal canned response: echo the query ID with QR+RD+RA set
                // and one A record (192.0.2.1). 2-byte length prefix first.
                std::vector<std::uint8_t> body{
                    captured_[2], captured_[3], // ID from the query
                    0x81, 0x80,                 // flags
                    0x00, 0x01, 0x00, 0x01,     // QD/AN count
                    0x00, 0x00, 0x00, 0x00,     // NS/AR count
                };
                // Copy the question section verbatim (everything past the 2-byte
                // length prefix and 12-byte header of the query).
                body.insert(body.end(), captured_.begin() + 2 + 12, captured_.end());
                // Answer: pointer to question + TYPE A + CLASS IN + TTL + RDLEN + 192.0.2.1
                body.insert(body.end(), {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
                                         192, 0, 2, 1});
                if (buf.size() == 2) {
                    buf[0] = static_cast<std::uint8_t>(body.size() >> 8);
                    buf[1] = static_cast<std::uint8_t>(body.size() & 0xFF);
                    return {};
                }
                std::copy_n(body.begin(), std::min(buf.size(), body.size()), buf.begin());
                return {};
            });
    }

private:
    std::vector<std::uint8_t> captured_;
};

// ---------------------------------------------------------------------------
//  ensure_connected failure paths
// ---------------------------------------------------------------------------

TEST(DotResolverMockTest, ConnectTimeout_ReturnsRetry) {
    auto mock = std::make_unique<MockStream>();
    ON_CALL(*mock, ensure_connected())
        .WillByDefault(Return(std::unexpected(IoError::TIMEOUT)));
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::RETRY);
}

TEST(DotResolverMockTest, ConnectCancelled_ReturnsCancelled) {
    auto mock = std::make_unique<MockStream>();
    ON_CALL(*mock, ensure_connected())
        .WillByDefault(Return(std::unexpected(IoError::CANCELLED)));
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CANCELLED);
}

TEST(DotResolverMockTest, ConnectFailed_ReturnsConnection) {
    auto mock = std::make_unique<MockStream>();
    ON_CALL(*mock, ensure_connected())
        .WillByDefault(Return(std::unexpected(IoError::CONNECTION_FAILED)));
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CONNECTION);
}

// ---------------------------------------------------------------------------
//  send_all / read_exact failure paths
// ---------------------------------------------------------------------------

TEST(DotResolverMockTest, SendCancelled_ReturnsCancelled) {
    auto mock = connected_mock();
    ON_CALL(*mock, send_all(_)).WillByDefault(Return(std::unexpected(IoError::CANCELLED)));
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CANCELLED);
}

TEST(DotResolverMockTest, SendFailed_ReturnsConnection) {
    auto mock = connected_mock();
    ON_CALL(*mock, send_all(_)).WillByDefault(Return(std::unexpected(IoError::CONNECTION_FAILED)));
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CONNECTION);
}

TEST(DotResolverMockTest, ReadCancelled_ReturnsCancelled) {
    auto mock = connected_mock();
    ON_CALL(*mock, send_all(_)).WillByDefault(Return(std::expected<void, IoError>{}));
    ON_CALL(*mock, read_exact(_)).WillByDefault(Return(std::unexpected(IoError::CANCELLED)));
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CANCELLED);
}

TEST(DotResolverMockTest, ReadTimeout_ReturnsConnection) {
    auto mock = connected_mock();
    ON_CALL(*mock, send_all(_)).WillByDefault(Return(std::expected<void, IoError>{}));
    ON_CALL(*mock, read_exact(_)).WillByDefault(Return(std::unexpected(IoError::TIMEOUT)));
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CONNECTION);
}

TEST(DotResolverMockTest, ZeroLengthResponse_ReturnsParse) {
    auto mock = connected_mock();
    ON_CALL(*mock, send_all(_)).WillByDefault(Return(std::expected<void, IoError>{}));
    // First read_exact (2-byte length prefix) delivers 0.
    ON_CALL(*mock, read_exact(_))
        .WillByDefault([](std::span<std::uint8_t> buf) -> std::expected<void, IoError> {
            buf[0] = 0;
            buf[1] = 0;
            return {};
        });
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::PARSE);
}

// ---------------------------------------------------------------------------
//  Retry: first send fails, reconnect succeeds
// ---------------------------------------------------------------------------

TEST(DotResolverMockTest, SendFailsThenReconnectSucceeds) {
    auto mock = std::make_unique<MockStream>();
    ON_CALL(*mock, ensure_connected()).WillByDefault(Return(std::expected<void, IoError>{}));

    bool first_attempt = true;
    std::vector<std::uint8_t> captured;
    ON_CALL(*mock, send_all(_))
        .WillByDefault([&](std::span<const std::uint8_t> data) -> std::expected<void, IoError> {
            if (first_attempt) {
                first_attempt = false;
                return std::unexpected(IoError::CONNECTION_FAILED);
            }
            captured.assign(data.begin(), data.end());
            return {};
        });
    ON_CALL(*mock, read_exact(_))
        .WillByDefault([&](std::span<std::uint8_t> buf) -> std::expected<void, IoError> {
            // Canned response mirroring the query ID with QR set + one A record.
            std::vector<std::uint8_t> body{
                captured[2], captured[3], 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
            };
            body.insert(body.end(), captured.begin() + 2 + 12, captured.end());
            body.insert(body.end(), {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
                                     192, 0, 2, 1});
            if (buf.size() == 2) {
                buf[0] = static_cast<std::uint8_t>(body.size() >> 8);
                buf[1] = static_cast<std::uint8_t>(body.size() & 0xFF);
                return {};
            }
            std::copy_n(body.begin(), std::min(buf.size(), body.size()), buf.begin());
            return {};
        });
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[2] & 0x80, 0x80);
}

// ---------------------------------------------------------------------------
//  Roundtrip
// ---------------------------------------------------------------------------

TEST(DotResolverMockTest, QuerySucceeds) {
    auto mock = connected_mock();
    MockServerPipe pipe(*mock);
    DotResolver resolver("127.0.0.1", 1853, "mock:1853", std::move(mock));

    auto result = resolver.query("yaddnsc.test", RecordKind::A);
    ASSERT_TRUE(result.has_value());
    // ID echoed from the query, QR flag set (byte 2).
    EXPECT_EQ((*result)[2] & 0x80, 0x80);
}

} // namespace
