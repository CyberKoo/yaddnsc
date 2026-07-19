//
// Unit tests for TlsStream error mapping (TlsConnectionBase → Transport::IoError).
//
// Verifies that TlsStream correctly maps each TlsConnectionBase::IoStatus
// value to the corresponding Transport::IoError, and that the success path
// passes through data unchanged.
// =============================================================================

#include <cstdint>
#include <span>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "network/transport/tls_stream.h"
#include "mocks/mock_tls_connection.h"

#include "util/cancellation_token.hpp"

using ::testing::_;
using ::testing::Return;

namespace {

// ── read_some error mapping ─────────────────────────────────────────────────

TEST(TlsStreamTest, ReadSome_Timeout_MapsToTimeout) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, read_some(_, _))
        .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::TIMEOUT)));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::array<std::uint8_t, 16> buf{};

    auto result = stream.read_some(buf, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::TIMEOUT);
}

TEST(TlsStreamTest, ReadSome_Cancelled_MapsToCancelled) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, read_some(_, _))
        .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::CANCELLED)));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::array<std::uint8_t, 16> buf{};

    auto result = stream.read_some(buf, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::CANCELLED);
}

TEST(TlsStreamTest, ReadSome_Error_MapsToConnectionFailed) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, read_some(_, _))
        .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::ERROR)));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::array<std::uint8_t, 16> buf{};

    auto result = stream.read_some(buf, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::CONNECTION_FAILED);
}

TEST(TlsStreamTest, ReadSome_Success_PassesData) {
    std::vector<std::uint8_t> expected = {0x01, 0x02, 0x03};
    MockTlsConnection mock;
    mock.set_read_data(expected);

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::array<std::uint8_t, 16> buf{};

    auto result = stream.read_some(buf, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, expected.size());
    EXPECT_EQ(std::vector<std::uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*result)),
              expected);
}

// ── read_exact error mapping ────────────────────────────────────────────────

TEST(TlsStreamTest, ReadExact_Timeout_MapsToTimeout) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, read_exact(_, _))
        .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::TIMEOUT)));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::array<std::uint8_t, 4> buf{};

    auto result = stream.read_exact(buf, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::TIMEOUT);
}

TEST(TlsStreamTest, ReadExact_Cancelled_MapsToCancelled) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, read_exact(_, _))
        .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::CANCELLED)));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::array<std::uint8_t, 4> buf{};

    auto result = stream.read_exact(buf, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::CANCELLED);
}

TEST(TlsStreamTest, ReadExact_Error_MapsToConnectionFailed) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, read_exact(_, _))
        .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::ERROR)));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::array<std::uint8_t, 4> buf{};

    auto result = stream.read_exact(buf, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::CONNECTION_FAILED);
}

TEST(TlsStreamTest, ReadExact_Success_PassesData) {
    std::vector<std::uint8_t> expected = {0xAA, 0xBB, 0xCC, 0xDD};
    MockTlsConnection mock;
    mock.set_read_data(expected);

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::array<std::uint8_t, 4> buf{};

    auto result = stream.read_exact(buf, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::vector<std::uint8_t>(buf.begin(), buf.end()), expected);
}

// ── send_all error mapping ─────────────────────────────────────────────────

TEST(TlsStreamTest, SendAll_Timeout_MapsToTimeout) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, send_all(_, _))
        .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::TIMEOUT)));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::vector<std::uint8_t> data(16);

    auto result = stream.send_all(data, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::TIMEOUT);
}

TEST(TlsStreamTest, SendAll_Cancelled_MapsToCancelled) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, send_all(_, _))
        .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::CANCELLED)));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::vector<std::uint8_t> data(16);

    auto result = stream.send_all(data, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::CANCELLED);
}

TEST(TlsStreamTest, SendAll_Error_MapsToConnectionFailed) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, send_all(_, _))
        .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::ERROR)));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::vector<std::uint8_t> data(16);

    auto result = stream.send_all(data, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::CONNECTION_FAILED);
}

TEST(TlsStreamTest, SendAll_Success) {
    MockTlsConnection mock;
    EXPECT_CALL(mock, send_all(_, _))
        .WillOnce(Return(std::expected<void, TlsConnectionBase::IoStatus>{}));

    Transport::TlsStream stream(mock);
    Utils::CancellationToken cancel;
    std::vector<std::uint8_t> data(16);

    auto result = stream.send_all(data, cancel);
    ASSERT_TRUE(result.has_value());
}

} // anonymous namespace
