//
// MockSocket — GoogleMock-based mock for SocketBase.
//
// Allows tests to simulate socket I/O outcomes (send, recv, connect
// timeouts, cancellation, etc.) without real system calls.
//
// Usage:
//   MockSocket mock;
//   EXPECT_CALL(mock, send_to(_, _))
//       .WillOnce(Return(ssize_t{4}));
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_SOCKET_H
#define YADDNSC_TEST_MOCKS_MOCK_SOCKET_H

#include <cstddef>
#include <span>

#include <gmock/gmock.h>

#include "network/socket.h"

class MockSocket : public SocketBase {
public:
    // NOTE: return types with commas must be wrapped in extra parentheses.

    MOCK_METHOD((std::expected<void, int>), set_option_raw,
                (int level, int optname, const void *val, socklen_t len),
                (const, noexcept, override));

    MOCK_METHOD((std::expected<void, int>), set_nonblocking, (bool enable), (const, noexcept, override));

    MOCK_METHOD((std::expected<void, ConnectError>), connect,
                (const SocketAddr& addr, int timeout_sec), (override));

    MOCK_METHOD(ssize_t, send, (std::span<const std::byte> data), (const, override));
    MOCK_METHOD(ssize_t, send, (std::span<const std::byte> data, int flags), (const, override));
    MOCK_METHOD(ssize_t, send_to, (std::span<const std::byte> data, const SocketAddr& dest), (const, override));
    MOCK_METHOD(ssize_t, send_to, (std::span<const std::byte> data, const SocketAddr& dest, int flags), (const, override));

    MOCK_METHOD(ssize_t, recv, (std::span<std::byte> buf), (const, override));
    MOCK_METHOD(ssize_t, recv, (std::span<std::byte> buf, int flags), (const, override));
    MOCK_METHOD(ssize_t, recv_from, (std::span<std::byte> buf, SocketAddr* src), (const, override));
    MOCK_METHOD(ssize_t, recv_from, (std::span<std::byte> buf, int flags, SocketAddr* src), (const, override));
    MOCK_METHOD(ssize_t, recv_exact, (std::span<std::byte> buf), (const, override));
    MOCK_METHOD(ssize_t, recv_exact, (std::span<std::byte> buf, int flags), (const, override));

    MOCK_METHOD(void, shutdown, (int how), (noexcept, override));
    MOCK_METHOD(void, close, (), (noexcept, override));

    MOCK_METHOD((std::expected<int, int>), wait_for,
                (short events, int timeout_ms), (const, noexcept, override));
    MOCK_METHOD((std::expected<int, int>), wait_for,
                (short events, int timeout_ms, const Utils::CancellationToken& cancel_token),
                (const, noexcept, override));

    MOCK_METHOD(int, native_handle, (), (const, noexcept, override));
    MOCK_METHOD(bool, is_closed, (), (const, noexcept, override));
};

#endif  // YADDNSC_TEST_MOCKS_MOCK_SOCKET_H
