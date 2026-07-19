//
// MockSocket unit tests — verifies MockSocket works correctly and
// demonstrates the pattern for using SocketBase in tests.
// =============================================================================

#include <cstddef>
#include <poll.h>
#include <span>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mocks/mock_socket.h"
#include "util/cancellation_token.hpp"

namespace {

using ::testing::_;
using ::testing::Return;

TEST(MockSocketTest, SendToMocked) {
    MockSocket mock;
    SocketAddr addr;  // default-constructed, unused in mock

    EXPECT_CALL(mock, send_to(_, _))
        .WillOnce(Return(ssize_t{4}));

    std::vector<std::byte> data(4);
    auto n = mock.send_to(data, addr);
    EXPECT_EQ(n, 4);
}

TEST(MockSocketTest, RecvFromMocked) {
    MockSocket mock;

    EXPECT_CALL(mock, recv_from(_, testing::IsNull()))
        .WillOnce([](std::span<std::byte> buf, SocketAddr*) {
            std::fill(buf.begin(), buf.begin() + 3, std::byte{0xAB});
            return ssize_t{3};
        });

    std::array<std::byte, 16> buf{};
    auto n = mock.recv_from(buf, nullptr);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(buf[0], std::byte{0xAB});
    EXPECT_EQ(buf[1], std::byte{0xAB});
    EXPECT_EQ(buf[2], std::byte{0xAB});
}

TEST(MockSocketTest, ConnectTimeout) {
    MockSocket mock;
    SocketAddr addr;

    EXPECT_CALL(mock, connect(_, _))
        .WillOnce(Return(std::unexpected(ConnectError::TIMED_OUT)));

    auto result = mock.connect(addr, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConnectError::TIMED_OUT);
}

TEST(MockSocketTest, SetOptionRawMocked) {
    MockSocket mock;
    int val = 1;

    EXPECT_CALL(mock, set_option_raw(SOL_SOCKET, SO_REUSEADDR, _, sizeof(int)))
        .WillOnce(Return(std::expected<void, int>{}));

    auto result = mock.set_option(SOL_SOCKET, SO_REUSEADDR, val);
    ASSERT_TRUE(result.has_value());
}

TEST(MockSocketTest, WaitForReturnsReady) {
    MockSocket mock;

    EXPECT_CALL(mock, wait_for(POLLIN, 1000, _))
        .WillOnce(Return(1));

    Utils::CancellationToken cancel;
    auto result = mock.wait_for(POLLIN, 1000, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1);
}

TEST(MockSocketTest, CloseAndIsClosed) {
    MockSocket mock;

    EXPECT_CALL(mock, is_closed())
        .WillOnce(Return(false))
        .WillOnce(Return(true));

    EXPECT_FALSE(mock.is_closed());
    EXPECT_TRUE(mock.is_closed());
}

TEST(MockSocketTest, SendSuccess) {
    MockSocket mock;

    EXPECT_CALL(mock, send(_))
        .WillOnce(Return(ssize_t{8}));

    std::vector<std::byte> data(8);
    auto n = mock.send(data);
    EXPECT_EQ(n, 8);
}

} // anonymous namespace
