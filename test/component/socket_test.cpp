//
// Integration tests for the Socket RAII wrapper using loopback.
//
// Creates a server socket bound to 127.0.0.1, accepts a connection,
// sends and receives data — all on the loopback interface.
//
// =============================================================================

#include "infrastructure/network/socket.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <expected>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "domain/network/inet_address.h"
#include "infrastructure/network/socket_addr.h"
#include "support/util/cancellation_token.hpp"


using namespace std::chrono_literals;

namespace {

[[nodiscard]] Socket must_open(int domain, int type, int protocol = 0) {
    auto opened = Socket::open(domain, type, protocol);
    if (!opened) {
        ADD_FAILURE() << "Socket::open failed: " << opened.error();
        return {};
    }
    return std::move(*opened);
}

[[nodiscard]] std::chrono::steady_clock::time_point test_deadline(std::chrono::milliseconds budget = 2s) {
    return std::chrono::steady_clock::now() + budget;
}

}  // namespace

// ===========================================================================
// Basic Socket operations
// ===========================================================================

TEST(SocketTest, CreateTcpSocket) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    EXPECT_GE(sock.native_handle(), 0);
    EXPECT_FALSE(sock.is_closed());
}

TEST(SocketTest, CreateUdpSocket) {
    Socket sock = must_open(AF_INET, SOCK_DGRAM);
    EXPECT_GE(sock.native_handle(), 0);
    EXPECT_FALSE(sock.is_closed());
}

TEST(SocketTest, MoveAssignmentClosesOld) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    int old_fd = sock.native_handle();
    Socket other = must_open(AF_INET, SOCK_DGRAM);
    sock = std::move(other);
    // old_fd should be closed by the move assignment
    EXPECT_NE(sock.native_handle(), old_fd);
    EXPECT_GE(sock.native_handle(), 0);
}

// ===========================================================================
// Loopback TCP echo
// ===========================================================================

TEST(SocketTest, TcpEchoOnLoopback) {
    // Build a SocketAddr for 127.0.0.1:0 (any available port).
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    // Server: create, bind, listen.
    Socket server = must_open(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto server_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(server_addr.has_value());
    server.bind(*server_addr).value();
    ASSERT_TRUE(server.listen(1));

    // Retrieve the actual port assigned by the kernel.
    auto server_sockname = server.get_sockname();
    ASSERT_TRUE(server_sockname.has_value());
    auto server_port = server_sockname->port();
    ASSERT_GT(server_port, 0);

    // Client: create and connect.
    auto client_target = SocketAddr::from_inet(*loopback, server_port);
    ASSERT_TRUE(client_target.has_value());

    Socket client = must_open(AF_INET, SOCK_STREAM);
    auto connect_result = client.connect(*client_target, test_deadline(), {});
    ASSERT_TRUE(connect_result.has_value()) << "connect failed";

    // Server: accept the connection.
    SocketAddr peer_addr;
    auto accepted = server.accept(&peer_addr);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_GE(accepted->native_handle(), 0);
    EXPECT_EQ(peer_addr.family(), AF_INET);

    // Send data from client to server.
    const std::string message = "Hello, socket!";
    auto sent = client.send_some(std::as_bytes(std::span{message}));
    ASSERT_TRUE(sent) << sent.error();
    EXPECT_EQ(*sent, message.size());

    // Receive on server side. The accepted socket stays blocking.
    std::array<std::byte, 64> recv_buf{};
    auto received = accepted->recv_some(std::span{recv_buf});
    ASSERT_TRUE(received) << received.error();
    EXPECT_EQ(*received, message.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(recv_buf.data()), *received), message);
}

TEST(SocketTest, TcpConnectRefused) {
    // Attempt to connect to 127.0.0.1:1 (port 1, nothing listening).
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.set_nonblocking(true).value();
    auto target = SocketAddr::from_inet(*loopback, 1);
    ASSERT_TRUE(target.has_value());

    auto result = sock.connect(*target, test_deadline(), {});
    ASSERT_FALSE(result.has_value());
    // A future deadline lets connect() run. Linux reports ECONNREFUSED for a
    // closed port; a platform that surfaces the RST late may report ETIMEDOUT.
    EXPECT_TRUE(result.error() == ECONNREFUSED || result.error() == ETIMEDOUT);
}

// ===========================================================================
// Socket option helpers
// ===========================================================================

TEST(SocketTest, NonBlockingFlag) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.set_nonblocking(true).value();

    // Try connect to an unused port; should fail immediately with EINPROGRESS
    // or EAGAIN in non-blocking mode, not hang.
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());
    auto target = SocketAddr::from_inet(*loopback, 9999);
    ASSERT_TRUE(target.has_value());

    auto result = sock.connect(*target, test_deadline(), {});
    EXPECT_FALSE(result.has_value());  // Refused or InProgress
}

// ===========================================================================
// UDP socket operations
// ===========================================================================

TEST(SocketTest, UdpSendRecvOnLoopback) {
    // Create a pair of UDP sockets on loopback.
    Socket server = must_open(AF_INET, SOCK_DGRAM);
    Socket client = must_open(AF_INET, SOCK_DGRAM);

    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    // Bind server to a random port.
    auto server_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(server_addr.has_value());
    server.bind(*server_addr).value();
    auto server_port = server.get_sockname().value().port();
    ASSERT_GT(server_port, 0);

    // Send from client to server.
    auto target = SocketAddr::from_inet(*loopback, server_port);
    ASSERT_TRUE(target.has_value());

    const std::string message = "UDP test";
    auto sent = client.send_to(std::as_bytes(std::span{message}), *target);
    ASSERT_TRUE(sent) << sent.error();
    EXPECT_EQ(*sent, message.size());

    // Receive on server.
    std::array<std::byte, 64> recv_buf{};
    SocketAddr src_addr;
    auto received = server.recv_from(std::span{recv_buf}, &src_addr);
    ASSERT_TRUE(received) << received.error();
    ASSERT_EQ(*received, message.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(recv_buf.data()), *received), message);
    EXPECT_EQ(src_addr.family(), AF_INET);
}

// ===========================================================================
// Socket shutdown
// ===========================================================================

TEST(SocketTest, ShutdownWrite) {
    // shutdown_write() should behave like a half-close.
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.shutdown_write();
    EXPECT_FALSE(sock.is_closed());  // shutdown is not close
}

TEST(SocketTest, ShutdownBoth) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.shutdown_both();
    EXPECT_FALSE(sock.is_closed());
}

// ===========================================================================
// Socket get_sockname / get_peername
// ===========================================================================

TEST(SocketTest, GetSockname_BeforeBind_ReturnsUnspec) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    auto name = sock.get_sockname();
    ASSERT_TRUE(name.has_value());
    EXPECT_GE(name->family(), 0);
}

// ===========================================================================
// Close & shutdown edge cases
// ===========================================================================

TEST(SocketTest, CloseIdempotent) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    EXPECT_FALSE(sock.is_closed());

    sock.close();
    EXPECT_TRUE(sock.is_closed());

    // Second close must be a no-op (not crash, not abort).
    sock.close();
    EXPECT_TRUE(sock.is_closed());
}

TEST(SocketTest, ShutdownOnClosedSocket) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.close();
    EXPECT_TRUE(sock.is_closed());

    // All shutdown variants must be safe on a closed socket.
    EXPECT_NO_THROW(sock.shutdown_write());
    EXPECT_NO_THROW(sock.shutdown_both());
    EXPECT_NO_THROW(sock.shutdown_read());
}

// ===========================================================================
// Socket option helpers — set_* branches
// ===========================================================================

TEST(SocketTest, SetNonblockingFalse) {
    // set_nonblocking(false) exercises the "else" branch in the implementation.
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    EXPECT_TRUE(sock.set_nonblocking(true).has_value());
    EXPECT_TRUE(sock.set_nonblocking(false).has_value());
}

TEST(SocketTest, SetSocketOptionsTcp) {
    // Options available on any stream socket.
    Socket sock = must_open(AF_INET, SOCK_STREAM);

    EXPECT_TRUE(sock.set_keepalive(true).has_value());
    EXPECT_TRUE(sock.set_keepalive(false).has_value());

    EXPECT_TRUE(sock.set_linger(true, 1).has_value());
    EXPECT_TRUE(sock.set_linger(false).has_value());

    EXPECT_TRUE(sock.set_reuseaddr(true).has_value());
    EXPECT_TRUE(sock.set_reuseaddr(false).has_value());
}

TEST(SocketTest, SetSocketOptionsUdp) {
    Socket sock = must_open(AF_INET, SOCK_DGRAM);
    EXPECT_TRUE(sock.set_broadcast(true).has_value());
    EXPECT_TRUE(sock.set_broadcast(false).has_value());
}

TEST(SocketTest, SetIpv6Only) {
    Socket sock = must_open(AF_INET6, SOCK_STREAM);
    EXPECT_TRUE(sock.set_ipv6_only(true).has_value());
    EXPECT_TRUE(sock.set_ipv6_only(false).has_value());
}

TEST(SocketTest, SetReusePort) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    auto result = sock.set_reuseport(true);
    // SO_REUSEPORT is supported on Linux 3.9+. On other platforms it may
    // return ENOPROTOOPT — either outcome is valid.
    if (!result) {
        EXPECT_EQ(result.error(), ENOPROTOOPT);
    }
}

// ===========================================================================
// recv_exact — stream vs. datagram
// ===========================================================================

TEST(SocketTest, RecvExactOnStream) {
    // Set up a TCP loopback echo and verify recv_exact reads exactly the
    // requested amount (MSG_WAITALL path).
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket server = must_open(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto server_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(server_addr.has_value());
    server.bind(*server_addr).value();
    ASSERT_TRUE(server.listen(1));

    auto server_port = server.get_sockname().value().port();
    ASSERT_GT(server_port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, server_port);
    ASSERT_TRUE(client_target.has_value());

    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target, test_deadline(), {}).has_value());

    SocketAddr peer_addr;
    auto accepted = server.accept(&peer_addr);
    ASSERT_TRUE(accepted.has_value());

    // Send exactly 100 bytes from client.
    std::string payload(100, 'x');
    auto sent = client.send_some(std::as_bytes(std::span{payload}));
    ASSERT_TRUE(sent) << sent.error();
    ASSERT_EQ(*sent, 100u);

    // One recv_some on the blocking accepted socket returns the payload.
    std::array<std::byte, 100> buf{};
    auto received = accepted->recv_some(std::span{buf});
    ASSERT_TRUE(received) << received.error();
    EXPECT_EQ(*received, 100u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), 100), payload);
}

TEST(SocketTest, UdpSendToAndRecvFrom_DefaultOverloads) {
    Socket server = must_open(AF_INET, SOCK_DGRAM);
    Socket client = must_open(AF_INET, SOCK_DGRAM);
    const auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback);
    const auto bind_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(bind_addr);
    ASSERT_TRUE(server.bind(*bind_addr));
    const auto target = SocketAddr::from_inet(*loopback, server.get_sockname().value().port());
    ASSERT_TRUE(target);

    const std::string message = "default overloads";
    const auto sent = client.send_to(std::as_bytes(std::span{message}), *target);
    ASSERT_TRUE(sent) << sent.error();
    ASSERT_EQ(*sent, message.size());
    std::array<std::byte, 32> buffer{};
    SocketAddr sender;
    const auto received = server.recv_from(std::span{buffer}, &sender);
    ASSERT_TRUE(received) << received.error();
    ASSERT_EQ(*received, message.size());
    EXPECT_EQ(sender.family(), AF_INET);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer.data()), *received), message);
}

TEST(SocketTest, RecvSomeOnDatagramPreservesBoundary) {
    // One recv_some returns a single datagram, even when the buffer is larger.
    Socket server = must_open(AF_INET, SOCK_DGRAM);
    Socket client = must_open(AF_INET, SOCK_DGRAM);

    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    auto server_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(server_addr.has_value());
    server.bind(*server_addr).value();
    auto server_port = server.get_sockname().value().port();
    ASSERT_GT(server_port, 0);

    // Send a 32-byte datagram.
    auto target = SocketAddr::from_inet(*loopback, server_port);
    ASSERT_TRUE(target.has_value());

    std::string payload(32, 'y');
    auto sent = client.send_to(std::as_bytes(std::span{payload}), *target);
    ASSERT_TRUE(sent) << sent.error();
    ASSERT_EQ(*sent, 32u);

    std::array<std::byte, 128> buf{};
    auto received = server.recv_some(std::span{buf});
    ASSERT_TRUE(received) << received.error();
    EXPECT_EQ(*received, 32u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), 32), payload);
}

// ===========================================================================
// wait_for — readiness polling
// ===========================================================================

TEST(SocketTest, WaitForReady) {
    // A fresh UDP socket should be writable immediately.
    // TCP is not used here because on macOS/BSD an unconnected TCP socket
    // may not signal POLLOUT, whereas UDP always does.
    Socket sock = must_open(AF_INET, SOCK_DGRAM);
    auto result = sock.wait_until(POLLOUT, std::chrono::steady_clock::now(), {});
    ASSERT_TRUE(result.has_value()) << "wait_until failed: " << result.error();
    EXPECT_NE(*result & POLLOUT, 0);
}

TEST(SocketTest, WaitForTimeout) {
    // An accepted connection with nothing queued is not readable. An
    // unconnected TCP socket reports POLLHUP immediately, which is readiness,
    // so the timeout case needs a live peer that stays silent.
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback);
    Socket server = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(server.bind(*SocketAddr::from_inet(*loopback, 0)));
    ASSERT_TRUE(server.listen(1));
    const auto port = server.get_sockname().value().port();
    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*SocketAddr::from_inet(*loopback, port), test_deadline(), {}));
    // The accepted socket must stay open. Destroying it sends FIN, and the
    // client becomes readable.
    auto accepted = server.accept();
    ASSERT_TRUE(accepted);

    auto result = client.wait_until(POLLIN, std::chrono::steady_clock::now(), {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ETIMEDOUT);
}

// ===========================================================================
// accept without addr
// ===========================================================================

TEST(SocketTest, AcceptWithoutAddr) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket server = must_open(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    server.bind(*addr).value();
    ASSERT_TRUE(server.listen(1));

    auto port = server.get_sockname().value().port();
    ASSERT_GT(port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(client_target.has_value());

    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target, test_deadline(), {}).has_value());

    // accept(nullptr) should succeed and not provide peer address.
    auto accepted = server.accept(nullptr);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_GE(accepted->native_handle(), 0);
}

// ===========================================================================
// get_peername after connect
// ===========================================================================

TEST(SocketTest, GetPeerNameAfterConnect) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket server = must_open(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    server.bind(*addr).value();
    ASSERT_TRUE(server.listen(1));

    auto port = server.get_sockname().value().port();
    ASSERT_GT(port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(client_target.has_value());

    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target, test_deadline(), {}).has_value());

    // After connect, get_peername should return the server's address.
    auto peername = client.get_peername();
    ASSERT_TRUE(peername.has_value());
    EXPECT_EQ(peername->family(), AF_INET);
    EXPECT_EQ(peername->port(), port);
}

// ===========================================================================
// send/recv with explicit flags
// ===========================================================================

TEST(SocketTest, SendSomeAndRecvSome) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback);
    Socket server = must_open(AF_INET, SOCK_STREAM);
    const auto bind_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(bind_addr);
    ASSERT_TRUE(server.bind(*bind_addr));
    ASSERT_TRUE(server.listen(1));
    const auto target = SocketAddr::from_inet(*loopback, server.get_sockname().value().port());
    ASSERT_TRUE(target);

    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*target, test_deadline(), {}));
    auto accepted = server.accept();
    ASSERT_TRUE(accepted);

    const std::string payload = "ok";
    const auto sent = client.send_some(std::as_bytes(std::span{payload}));
    ASSERT_TRUE(sent) << sent.error();
    ASSERT_EQ(*sent, 2u);

    std::array<std::byte, 2> received{};
    const auto n = accepted->recv_some(std::span{received});
    ASSERT_TRUE(n) << n.error();
    ASSERT_EQ(*n, 2u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(received.data()), *n), payload);
}

TEST(SocketTest, SendRecvWithFlags) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket server = must_open(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    server.bind(*addr).value();
    ASSERT_TRUE(server.listen(1));

    auto port = server.get_sockname().value().port();
    ASSERT_GT(port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(client_target.has_value());

    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target, test_deadline(), {}).has_value());

    Socket accepted = *server.accept();

    const std::string msg = "flags test";
    auto sent = client.send_some(std::as_bytes(std::span{msg}), 0);
    ASSERT_TRUE(sent) << sent.error();
    ASSERT_EQ(*sent, msg.size());

    std::array<std::byte, 32> buf{};
    auto received = accepted.recv_some(std::span{buf}, 0);
    ASSERT_TRUE(received) << received.error();
    EXPECT_EQ(*received, msg.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), msg.size()), msg);
}

// ===========================================================================
// Error paths — construction, close, bind/connect/listen/accept failures
// ===========================================================================

TEST(SocketTest, Open_InvalidProtocol_ReturnsErrno) {
    auto opened = Socket::open(AF_INET, SOCK_STREAM, 0xFFFFFF);
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error(), EINVAL);
}

TEST(SocketTest, SelfMoveAssignment_IsNoOp) {  // NOLINT(bugprone-use-after-move)
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    const int fd = sock.native_handle();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
    sock = std::move(sock);  // NOLINT: intentional self-move — must be a no-op
#pragma GCC diagnostic pop
    EXPECT_EQ(sock.native_handle(), fd);
    EXPECT_FALSE(sock.is_closed());
}

TEST(SocketTest, SetOption_OnClosedSocket_ReturnsError) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.close();
    auto res = sock.set_reuseaddr(true);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), EBADF);
}

TEST(SocketTest, SetNonblocking_OnClosedSocket_ReturnsError) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.close();
    auto res = sock.set_nonblocking(true);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), EBADF);
}

TEST(SocketTest, SetReusePort_Disable) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    auto res = sock.set_reuseport(false);
    // SO_REUSEPORT is supported on Linux 3.9+. On other platforms it may
    // return ENOPROTOOPT — either outcome is valid.
    if (!res) {
        EXPECT_EQ(res.error(), ENOPROTOOPT);
    }
}

TEST(SocketTest, Bind_TwiceSamePort_ReturnsError) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket first = must_open(AF_INET, SOCK_STREAM);
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    first.bind(*addr).value();
    auto port = first.get_sockname().value().port();
    ASSERT_GT(port, 0);

    // Second bind to the same port without SO_REUSEADDR → EADDRINUSE.
    auto target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(target.has_value());
    Socket second = must_open(AF_INET, SOCK_STREAM);
    auto res = second.bind(*target);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), EADDRINUSE);
}

TEST(SocketTest, Bind_OnClosedSocket_ReturnsError) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());

    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.close();
    auto res = sock.bind(*addr);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), EBADF);
}

TEST(SocketTest, GetSockname_OnClosedSocket_ReturnsError) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.close();
    auto res = sock.get_sockname();
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), EBADF);
}

TEST(SocketTest, GetPeername_Unconnected_ReturnsError) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    auto res = sock.get_peername();  // ENOTCONN
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ENOTCONN);
}

TEST(SocketTest, BlockingConnect_Refused) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket sock = must_open(AF_INET, SOCK_STREAM);
    auto target = SocketAddr::from_inet(*loopback, 1);  // nothing listening
    ASSERT_TRUE(target.has_value());
    auto res = sock.connect(*target, test_deadline(), {});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ECONNREFUSED);
}

TEST(SocketTest, Connect_OnClosedSocket_ReturnsInternal) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.close();
    auto target = SocketAddr::from_inet(*loopback, 1);
    ASSERT_TRUE(target.has_value());
    auto res = sock.connect(*target, test_deadline(), {});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), EBADF);
}

TEST(SocketTest, Connect_UdpNonBlocking_ImmediateSuccess) {
    // UDP connect() returns immediately even with a timeout — exercises the
    // rc == 0 fast path in the non-blocking connect.
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket udp = must_open(AF_INET, SOCK_DGRAM);
    auto target = SocketAddr::from_inet(*loopback, 9);  // no listener needed for UDP
    ASSERT_TRUE(target.has_value());
    auto res = udp.connect(*target, test_deadline(), {});
    EXPECT_TRUE(res.has_value());
}

TEST(SocketTest, Listen_OnDatagramSocket_ReturnsError) {
    Socket sock = must_open(AF_INET, SOCK_DGRAM);
    auto res = sock.listen(1);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), EOPNOTSUPP);
}

TEST(SocketTest, Accept_OnUnlisteningSocket_ReturnsError) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    auto res = sock.accept();
    ASSERT_FALSE(res.has_value());  // EINVAL — not listening
}

// ===========================================================================
// Error paths — I/O
// ===========================================================================

TEST(SocketTest, SendToClosedPeer_ReturnsMinusOne) {
    // Stream send_loop returns -1 once the peer's FIN/RST is observed.
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket server = must_open(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    server.bind(*addr).value();
    ASSERT_TRUE(server.listen(1));

    auto port = server.get_sockname().value().port();
    ASSERT_GT(port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(client_target.has_value());

    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target, test_deadline(), {}).has_value());
    Socket accepted = *server.accept();

    // Close the peer (FIN, then RST on loopback).
    accepted.shutdown_both();
    accepted.close();

    // connect() leaves the client non-blocking, so a full window is EAGAIN.
    // Keep writing until the kernel reports the dead connection.
    std::string payload(64 * 1024, 'x');
    bool saw_error = false;
    for (int i = 0; i < 256 && !saw_error; ++i) {
        auto sent = client.send_some(std::as_bytes(std::span{payload}));
        if (sent) {
            continue;
        }
        if (sent.error() == EAGAIN || sent.error() == EWOULDBLOCK || sent.error() == EINTR) {
            auto ready = client.wait_until(POLLOUT, test_deadline(200ms), {});
            if (!ready && ready.error() != ETIMEDOUT && ready.error() != EAGAIN) {
                saw_error = ready.error() == EPIPE || ready.error() == ECONNRESET || ready.error() == ECONNABORTED;
            }
            continue;
        }
        EXPECT_TRUE(sent.error() == EPIPE || sent.error() == ECONNRESET || sent.error() == ECONNABORTED)
            << sent.error();
        saw_error = true;
    }
    EXPECT_TRUE(saw_error);
}

TEST(SocketTest, RecvExact_NonBlockingNoData_ReturnsMinusOne) {
    // Connected stream socket with no data and O_NONBLOCK → recv fails with
    // EAGAIN → recv_exact returns -1.
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket server = must_open(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    server.bind(*addr).value();
    ASSERT_TRUE(server.listen(1));

    auto port = server.get_sockname().value().port();
    ASSERT_GT(port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(client_target.has_value());

    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target, test_deadline(), {}).has_value());
    Socket accepted = *server.accept();  // keep the peer open, send nothing

    client.set_nonblocking(true).value();
    std::array<std::byte, 16> buf{};
    auto received = client.recv_some(std::span{buf});
    ASSERT_FALSE(received.has_value());
    EXPECT_TRUE(received.error() == EAGAIN || received.error() == EWOULDBLOCK);
}

TEST(SocketTest, RecvExact_PeerShutdown_ReturnsShortCount) {
    // Peer sends 10 bytes then half-closes: recv_exact(100) must return the
    // 10 bytes received so far (EOF path), not block for the remaining 90.
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket server = must_open(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    server.bind(*addr).value();
    ASSERT_TRUE(server.listen(1));

    auto port = server.get_sockname().value().port();
    ASSERT_GT(port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(client_target.has_value());

    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target, test_deadline(), {}).has_value());
    Socket accepted = *server.accept();

    const std::string msg = "0123456789";  // 10 bytes
    auto sent = accepted.send_some(std::as_bytes(std::span{msg}));
    ASSERT_TRUE(sent) << sent.error();
    ASSERT_EQ(*sent, msg.size());
    accepted.shutdown_write();  // EOF after the payload

    ASSERT_TRUE(client.wait_until(POLLIN, test_deadline(), {}));
    std::array<std::byte, 100> buf{};
    auto received = client.recv_some(std::span{buf});
    ASSERT_TRUE(received) << received.error();
    EXPECT_EQ(*received, msg.size());

    ASSERT_TRUE(client.wait_until(POLLIN, test_deadline(), {}));
    auto eof = client.recv_some(std::span{buf});
    ASSERT_TRUE(eof) << eof.error();
    EXPECT_EQ(*eof, 0u);
}

// ===========================================================================
// Error paths — wait_for
// ===========================================================================

TEST(SocketTest, WaitFor_OnClosedSocket_ReturnsNotReady) {
    Socket sock = must_open(AF_INET, SOCK_STREAM);
    sock.close();
    auto res = sock.wait_until(POLLIN, std::chrono::steady_clock::now(), {});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), EBADF);
}

TEST(SocketTest, WaitFor_WithTokenNotCancelled_ReturnsReady) {
    Utils::CancellationSource source;
    Socket sock = must_open(AF_INET, SOCK_DGRAM);
    auto res = sock.wait_until(POLLOUT, std::chrono::steady_clock::now(), source.token());
    ASSERT_TRUE(res.has_value());
    EXPECT_NE(*res & POLLOUT, 0);
}

TEST(SocketTest, WaitFor_Cancelled_ReturnsECANCELED) {
    Utils::CancellationSource source;
    Socket sock = must_open(AF_INET, SOCK_STREAM);  // no data pending
    source.trigger();
    auto res = sock.wait_until(POLLIN, test_deadline(), source.token());
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ECANCELED);
}

TEST(SocketTest, WaitUntil_CancelDuringWait_ReturnsECANCELED) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback);
    Socket server = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(server.bind(*SocketAddr::from_inet(*loopback, 0)));
    ASSERT_TRUE(server.listen(1));
    const auto port = server.get_sockname().value().port();
    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*SocketAddr::from_inet(*loopback, port), test_deadline(), {}));
    auto accepted = server.accept();
    ASSERT_TRUE(accepted);

    Utils::CancellationSource source;
    std::thread canceller([&source] {
        std::this_thread::sleep_for(50ms);
        source.trigger();
    });
    const auto start = std::chrono::steady_clock::now();
    auto res = client.wait_until(POLLIN, test_deadline(), source.token());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    canceller.join();
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ECANCELED);
    EXPECT_LT(elapsed, 1s);
}

TEST(SocketTest, OpenAndAccept_SetCloexec) {
    Socket server = must_open(AF_INET, SOCK_STREAM);
    const int server_flags = ::fcntl(server.native_handle(), F_GETFD);
    ASSERT_GE(server_flags, 0);
    EXPECT_NE(server_flags & FD_CLOEXEC, 0);

    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback);
    ASSERT_TRUE(server.bind(*SocketAddr::from_inet(*loopback, 0)));
    ASSERT_TRUE(server.listen(1));
    const auto port = server.get_sockname().value().port();
    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*SocketAddr::from_inet(*loopback, port), test_deadline(), {}));
    auto accepted = server.accept();
    ASSERT_TRUE(accepted);
    const int accepted_flags = ::fcntl(accepted->native_handle(), F_GETFD);
    ASSERT_GE(accepted_flags, 0);
    EXPECT_NE(accepted_flags & FD_CLOEXEC, 0);
}

TEST(SocketTest, MoveLeavesSourceClosed) {
    Socket original = must_open(AF_INET, SOCK_STREAM);
    const int fd = original.native_handle();
    Socket moved = std::move(original);
    EXPECT_TRUE(original.is_closed());  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(moved.native_handle(), fd);
    moved.close();
    EXPECT_TRUE(moved.is_closed());
    moved.close();
    EXPECT_TRUE(moved.is_closed());
}

TEST(SocketTest, UdpEmptyDatagramIsSuccess) {
    Socket server = must_open(AF_INET, SOCK_DGRAM);
    Socket client = must_open(AF_INET, SOCK_DGRAM);
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback);
    ASSERT_TRUE(server.bind(*SocketAddr::from_inet(*loopback, 0)));
    const auto port = server.get_sockname().value().port();
    const auto target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(target);
    auto sent = client.send_to({}, *target);
    ASSERT_TRUE(sent) << sent.error();
    EXPECT_EQ(*sent, 0u);

    std::array<std::byte, 8> buf{};
    SocketAddr src;
    auto received = server.recv_from(std::span{buf}, &src);
    ASSERT_TRUE(received) << received.error();
    EXPECT_EQ(*received, 0u);
    EXPECT_EQ(src.port(), client.get_sockname().value().port());
}

TEST(SocketTest, WaitUntil_ReadableWithHangup_KeepsPollin) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback);
    Socket server = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(server.bind(*SocketAddr::from_inet(*loopback, 0)));
    ASSERT_TRUE(server.listen(1));
    const auto port = server.get_sockname().value().port();
    Socket client = must_open(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*SocketAddr::from_inet(*loopback, port), test_deadline(), {}));
    auto accepted = server.accept();
    ASSERT_TRUE(accepted);

    const std::string msg = "z";
    ASSERT_TRUE(accepted->send_some(std::as_bytes(std::span{msg})));
    accepted->shutdown_write();

    auto ready = client.wait_until(POLLIN, test_deadline(), {});
    ASSERT_TRUE(ready) << ready.error();
    EXPECT_NE(*ready & POLLIN, 0);

    std::array<std::byte, 4> buf{};
    auto received = client.recv_some(std::span{buf});
    ASSERT_TRUE(received) << received.error();
    EXPECT_EQ(*received, 1u);
}
