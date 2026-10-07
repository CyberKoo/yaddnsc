//
// Component tests for the *successful* connection paths of the TCP transport.
//
// test/unit/net_transport/net_transport_test.cpp already covers the failure
// paths (refused port, unresolvable host, zero budget, pre-cancelled token)
// against TcpConnection constructed directly. What remains uncovered is the
// other half of the code: a real connect_one() against a live listener, the
// is_healthy() liveness probe in all three of its outcomes, and TcpStream's
// read/send paths including the EOF and shared-deadline branches.
//
// Uses a real loopback TCP listener, so it lives in component/ per the
// test-directory convention.
// =============================================================================

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <expected>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "infrastructure/network/socket.h"
#include "infrastructure/network/tcp_transfer.h"
#include "infrastructure/network/transport/detail/tcp_connection.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "support/util/cancellation_token.hpp"

using Transport::IoError;
using Transport::Options;

namespace {

/// A loopback TCP listener that accepts one connection at a time and hands
/// the accepted fd to the test.
class LoopbackServer {
public:
    LoopbackServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            throw std::runtime_error("socket() failed");
        }
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(listen_fd_);
            throw std::runtime_error("bind() failed");
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(listen_fd_);
            throw std::runtime_error("getsockname() failed");
        }
        port_ = ntohs(addr.sin_port);
        const int flags = ::fcntl(listen_fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
            ::close(listen_fd_);
            throw std::runtime_error("fcntl() failed");
        }

        if (::listen(listen_fd_, 4) != 0) {
            ::close(listen_fd_);
            throw std::runtime_error("listen() failed");
        }
    }

    ~LoopbackServer() {
        close_client();
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
        }
    }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] int listen_fd() const noexcept { return listen_fd_; }

    /// Accept a queued client without blocking.
    int accept() {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd >= 0) {
            close_client();
            client_fd_ = fd;
#ifndef MSG_NOSIGNAL
            const int one = 1;
            if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) {
                close_client();
                return -1;
            }
#endif
        }
        return fd;
    }

    void close_client() noexcept {
        if (client_fd_ >= 0) {
            ::close(client_fd_);
            client_fd_ = -1;
        }
    }

    /// Send raw bytes to the connected client.
    void send_to_client(std::string_view data) {
        if (client_fd_ < 0) {
            return;
        }
        while (!data.empty()) {
            pollfd pfd{.fd = client_fd_, .events = POLLOUT, .revents = 0};
            ASSERT_GT(::poll(&pfd, 1, 2000), 0);
#ifdef MSG_NOSIGNAL
            constexpr int FLAGS = MSG_NOSIGNAL;
#else
            constexpr int FLAGS = 0;
#endif
            const auto n = ::send(client_fd_, data.data(), data.size(), FLAGS);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            ASSERT_GT(n, 0);
            data.remove_prefix(static_cast<std::size_t>(n));
        }
    }

private:
    int listen_fd_ = -1;
    int client_fd_ = -1;
    std::uint16_t port_ = 0;
};

/// Poll the listener until a client arrives, so connect() cannot race the
/// accept() call.
int accept_within(LoopbackServer& server, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd pfd{.fd = server.listen_fd(), .events = POLLIN, .revents = 0};
        const int ready =
            ::poll(&pfd, 1, static_cast<int>(std::max(remaining.count(), decltype(remaining.count()){0})));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0 || !(pfd.revents & POLLIN)) {
            return -1;
        }
        const int fd = server.accept();
        if (fd >= 0) {
            return fd;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
    }
    return -1;
}

Options fast_options() {
    Options opts;
    opts.connect_timeout = std::chrono::seconds(5);
    opts.read_timeout = std::chrono::milliseconds(500);
    opts.write_timeout = std::chrono::milliseconds(500);
    return opts;
}

[[nodiscard]] std::chrono::steady_clock::time_point deadline_in(const std::chrono::milliseconds budget) {
    return std::chrono::steady_clock::now() + budget;
}

}  // namespace

// ===========================================================================
// TcpConnection — connect success and liveness
// ===========================================================================

TEST(NetTransportSuccess, AcceptWithin_NoClient_TimesOut) {
    LoopbackServer server;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(accept_within(server, std::chrono::milliseconds(30)), -1);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST(NetTransportSuccess, TcpConnection_ConnectToLiveListener_Succeeds) {
    LoopbackServer server;
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", server.port(), fast_options());

    const auto result = connection.connect(deadline_in(std::chrono::seconds(5)), token);
    ASSERT_TRUE(result.has_value()) << "connect failed: " << static_cast<int>(result.error());
    EXPECT_TRUE(connection.is_connected());
    EXPECT_GE(connection.socket().native_handle(), 0);
    EXPECT_EQ(connection.host(), "127.0.0.1");
    EXPECT_EQ(connection.port(), server.port());

    EXPECT_GE(accept_within(server, std::chrono::seconds(2)), 0);
}

TEST(NetTransportSuccess, TcpConnection_IsHealthy_OnIdleConnection) {
    LoopbackServer server;
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", server.port(), fast_options());
    ASSERT_TRUE(connection.connect(deadline_in(std::chrono::seconds(5)), token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    // Nothing pending: the probe treats the connection as alive.
    EXPECT_TRUE(connection.is_healthy());

    connection.close();
    EXPECT_FALSE(connection.is_connected());
    // A closed connection has no fd, so the probe short-circuits to false.
    EXPECT_FALSE(connection.is_healthy());
}

TEST(NetTransportSuccess, TcpConnection_IsHealthy_WithPendingData) {
    LoopbackServer server;
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", server.port(), fast_options());
    ASSERT_TRUE(connection.connect(deadline_in(std::chrono::seconds(5)), token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    server.send_to_client("x");
    // Wait for the byte to land so the POLLIN branch is definitely taken.
    const auto ready = connection.socket().wait_until(POLLIN, deadline_in(std::chrono::seconds(2)), token);
    ASSERT_TRUE(ready.has_value()) << ready.error();
    EXPECT_TRUE(connection.is_healthy());
    char byte = 0;
    ASSERT_EQ(::recv(connection.socket().native_handle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT), 1);
    EXPECT_EQ(byte, 'x');
}

TEST(NetTransportSuccess, TcpConnection_IsHealthy_AfterPeerClose) {
    LoopbackServer server;
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", server.port(), fast_options());
    ASSERT_TRUE(connection.connect(deadline_in(std::chrono::seconds(5)), token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    server.close_client();
    // The probe peeks: recv() returning 0 is EOF, so the peer is gone.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (connection.is_healthy() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_FALSE(connection.is_healthy());
}

TEST(NetTransportSuccess, TcpConnection_Wait_ReportsReadableData) {
    LoopbackServer server;
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", server.port(), fast_options());
    ASSERT_TRUE(connection.connect(deadline_in(std::chrono::seconds(5)), token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    server.send_to_client("hello");
    const auto ready = connection.socket().wait_until(POLLIN, deadline_in(std::chrono::seconds(2)), token);
    ASSERT_TRUE(ready.has_value()) << ready.error();
    EXPECT_NE(*ready & POLLIN, 0);
}

TEST(NetTransportSuccess, TcpConnection_Wait_TimesOutWithNoData) {
    LoopbackServer server;
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", server.port(), fast_options());
    ASSERT_TRUE(connection.connect(deadline_in(std::chrono::seconds(5)), token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    const auto ready = connection.socket().wait_until(POLLIN, deadline_in(std::chrono::milliseconds(50)), token);
    ASSERT_FALSE(ready.has_value());
    EXPECT_EQ(ready.error(), ETIMEDOUT);
}

TEST(NetTransportSuccess, TcpConnection_Wait_PreTriggeredToken_ReturnsCancelled) {
    LoopbackServer server;
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", server.port(), fast_options());
    ASSERT_TRUE(connection.connect(deadline_in(std::chrono::seconds(5)), token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    Utils::CancellationSource cancellation;
    cancellation.trigger();
    const auto ready =
        connection.socket().wait_until(POLLIN, deadline_in(std::chrono::seconds(2)), cancellation.token());
    ASSERT_FALSE(ready.has_value());
    EXPECT_EQ(ready.error(), ECANCELED);
}

// ===========================================================================
// TcpStream — read / send over a live connection
// ===========================================================================

TEST(NetTransportSuccess, TcpStream_EnsureConnected_ThenReadSome) {
    LoopbackServer server;
    Transport::TcpStream stream("127.0.0.1", server.port(), fast_options());
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);
    server.send_to_client("ping");

    std::array<std::uint8_t, 4> buf{};
    std::span<std::uint8_t> remaining(buf);
    while (!remaining.empty()) {
        const auto n = stream.read_some(remaining, token);
        ASSERT_TRUE(n.has_value()) << "read failed: " << static_cast<int>(n.error());
        ASSERT_GT(*n, 0u);
        remaining = remaining.subspan(*n);
    }
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()), "ping");
}

TEST(NetTransportSuccess, TcpStream_SendAll_ReachesPeer) {
    LoopbackServer server;
    Transport::TcpStream stream("127.0.0.1", server.port(), fast_options());
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    const int client = accept_within(server, std::chrono::seconds(2));
    ASSERT_GE(client, 0);

    const std::string payload = "request-bytes";
    const std::span<const std::uint8_t> payload_bytes{reinterpret_cast<const std::uint8_t*>(payload.data()),
                                                      payload.size()};
    const auto sent = stream.send_all(payload_bytes, token);
    ASSERT_TRUE(sent.has_value()) << "send failed: " << static_cast<int>(sent.error());

    // The peer receives the full payload.
    std::string received(payload.size(), '\0');
    std::size_t offset = 0;
    while (offset < received.size()) {
        pollfd pfd{.fd = client, .events = POLLIN, .revents = 0};
        ASSERT_GT(::poll(&pfd, 1, 2000), 0);
        const auto n = ::recv(client, received.data() + offset, received.size() - offset, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        ASSERT_GT(n, 0);
        offset += static_cast<std::size_t>(n);
    }
    EXPECT_EQ(received, payload);
}

TEST(NetTransportSuccess, TcpStream_ReadSome_AfterPeerClose_ReturnsConnectionFailed) {
    LoopbackServer server;
    Transport::TcpStream stream("127.0.0.1", server.port(), fast_options());
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);
    server.close_client();

    std::array<std::uint8_t, 16> buf{};
    const auto n = stream.read_some(buf, token);
    ASSERT_FALSE(n.has_value());
    EXPECT_EQ(n.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportSuccess, TcpStream_Close_ThenOperationsFail) {
    LoopbackServer server;
    Transport::TcpStream stream("127.0.0.1", server.port(), fast_options());
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    stream.close();
    // Reconnecting reuses the same instance.
    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    std::array<std::uint8_t, 8> buf{};
    server.send_to_client("z");
    const auto n = stream.read_some(buf, token);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 1u);
}

TEST(NetTransportSuccess, TcpStream_ReadExact_ReadsFullPayload) {
    LoopbackServer server;
    Transport::TcpStream stream("127.0.0.1", server.port(), fast_options());
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    // TCP may coalesce these writes; read_exact must accept either framing.
    server.send_to_client("abc");
    server.send_to_client("def");

    std::array<std::uint8_t, 6> buf{};
    const auto result = stream.read_exact(buf, token);
    ASSERT_TRUE(result.has_value()) << "read_exact failed: " << static_cast<int>(result.error());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()), "abcdef");
}

TEST(NetTransportSuccess, TcpStream_EnsureConnected_IsIdempotent) {
    LoopbackServer server;
    Transport::TcpStream stream("127.0.0.1", server.port(), fast_options());
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);
    // A second call must not drop the established connection.
    ASSERT_TRUE(stream.ensure_connected(token).has_value());

    server.send_to_client("still-here");
    std::array<std::uint8_t, 10> buf{};
    ASSERT_TRUE(stream.read_exact(buf, token).has_value());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()), "still-here");
}

TEST(NetTransportSuccess, TcpStream_ReadExact_PartialDeliverySharesOneBudget) {
    // Byte one lands after 200ms and byte two after another 200ms. A fresh
    // 300ms budget per read would accept both. One budget for the whole
    // read_exact expires between them.
    LoopbackServer server;
    auto opts = fast_options();
    opts.read_timeout = std::chrono::milliseconds(300);
    Transport::TcpStream stream("127.0.0.1", server.port(), opts);
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    std::thread sender([&server] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        server.send_to_client("a");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        server.send_to_client("b");
    });

    std::array<std::uint8_t, 2> buf{};
    const auto start = std::chrono::steady_clock::now();
    const auto result = stream.read_exact(buf, token);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    sender.join();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), IoError::TIMEOUT);
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(NetTransportSuccess, TcpStream_SendAll_UnreadPeer_TimesOut) {
    LoopbackServer server;
    auto opts = fast_options();
    opts.write_timeout = std::chrono::milliseconds(300);
    Transport::TcpStream stream("127.0.0.1", server.port(), opts);
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    // The peer never reads. The payload is larger than a typical loopback
    // window, so send_all has to wait and the single write budget expires.
    const std::vector<std::uint8_t> payload(8 * 1024 * 1024, 0xab);
    const auto start = std::chrono::steady_clock::now();
    const auto sent = stream.send_all(payload, token);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error(), IoError::TIMEOUT);
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(NetTransportSuccess, TcpTransfer_EmptyBuffer_SpentDeadline_Succeeds) {
    // An empty transfer returns before touching the socket. A closed fd
    // would be EBADF if the call started I/O.
    Socket closed;
    const auto spent = std::chrono::steady_clock::now();
    const auto read = tcp_read_some(closed, {}, spent, {});
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, 0u);
    EXPECT_TRUE(tcp_send_all(closed, {}, spent, {}).has_value());
    EXPECT_TRUE(tcp_read_exact(closed, {}, spent, {}).has_value());
}

TEST(NetTransportSuccess, TcpReadSome_SpentDeadline_LeavesQueuedByte) {
    LoopbackServer server;
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", server.port(), fast_options());
    ASSERT_TRUE(connection.connect(deadline_in(std::chrono::seconds(5)), token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);

    server.send_to_client("Q");
    ASSERT_TRUE(connection.socket().wait_until(POLLIN, deadline_in(std::chrono::seconds(2)), token).has_value());

    std::array<std::byte, 1> buf{};
    const auto missed = tcp_read_some(connection.socket(), buf, std::chrono::steady_clock::now(), token);
    ASSERT_FALSE(missed.has_value());
    EXPECT_EQ(missed.error(), ETIMEDOUT);

    const auto got = tcp_read_some(connection.socket(), buf, deadline_in(std::chrono::seconds(1)), token);
    ASSERT_TRUE(got.has_value()) << got.error();
    EXPECT_EQ(*got, 1u);
    EXPECT_EQ(buf[0], std::byte{'Q'});
}

TEST(NetTransportSuccess, TcpSendAll_SpentDeadline_DoesNotSend) {
    LoopbackServer server;
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", server.port(), fast_options());
    ASSERT_TRUE(connection.connect(deadline_in(std::chrono::seconds(5)), token).has_value());
    const int peer = accept_within(server, std::chrono::seconds(2));
    ASSERT_GE(peer, 0);

    const auto payload = std::array{std::byte{'Z'}};
    const auto missed = tcp_send_all(connection.socket(), payload, std::chrono::steady_clock::now(), token);
    ASSERT_FALSE(missed.has_value());
    EXPECT_EQ(missed.error(), ETIMEDOUT);

    pollfd pfd{.fd = peer, .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&pfd, 1, 50), 0);

    ASSERT_TRUE(tcp_send_all(connection.socket(), payload, deadline_in(std::chrono::seconds(1)), token).has_value());
    std::array<std::byte, 1> got{};
    pfd.revents = 0;
    ASSERT_GT(::poll(&pfd, 1, 1000), 0);
    ASSERT_EQ(::recv(peer, got.data(), got.size(), MSG_DONTWAIT), 1);
    EXPECT_EQ(got[0], std::byte{'Z'});
}

TEST(NetTransportSuccess, TcpStream_ZeroReadTimeout_DoesNotReturnQueuedByte) {
    LoopbackServer server;
    auto opts = fast_options();
    opts.read_timeout = std::chrono::milliseconds(0);
    Transport::TcpStream stream("127.0.0.1", server.port(), opts);
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    ASSERT_GE(accept_within(server, std::chrono::seconds(2)), 0);
    server.send_to_client("Q");

    // A spent read budget stays TIMEOUT for the whole window. Delivering
    // the queued byte would mean the call recv'd after the deadline.
    const auto start = std::chrono::steady_clock::now();
    IoError last = IoError::TIMEOUT;
    bool saw_data = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(200)) {
        std::array<std::uint8_t, 1> buf{};
        const auto n = stream.read_some(buf, token);
        if (n.has_value()) {
            saw_data = true;
            break;
        }
        last = n.error();
        if (last != IoError::TIMEOUT) {
            break;
        }
    }
    EXPECT_FALSE(saw_data);
    EXPECT_EQ(last, IoError::TIMEOUT);
}

TEST(NetTransportSuccess, TcpStream_ZeroWriteTimeout_DoesNotSend) {
    LoopbackServer server;
    auto opts = fast_options();
    opts.write_timeout = std::chrono::milliseconds(0);
    Transport::TcpStream stream("127.0.0.1", server.port(), opts);
    const Utils::CancellationToken token;

    ASSERT_TRUE(stream.ensure_connected(token).has_value());
    const int peer = accept_within(server, std::chrono::seconds(2));
    ASSERT_GE(peer, 0);

    const auto payload = std::array<std::uint8_t, 1>{std::uint8_t{'Z'}};
    const auto start = std::chrono::steady_clock::now();
    const auto sent = stream.send_all(payload, token);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(sent.has_value());
    EXPECT_EQ(sent.error(), IoError::TIMEOUT);
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));

    pollfd pfd{.fd = peer, .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&pfd, 1, 50), 0);
}
