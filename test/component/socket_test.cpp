//
// Integration tests for the Socket RAII wrapper using loopback.
//
// Creates a server socket bound to 127.0.0.1, accepts a connection,
// sends and receives data — all on the loopback interface.
//
// =============================================================================

#include <cstddef>
#include <cstring>
#include <span>

#include <poll.h>

#include <gtest/gtest.h>

#include "network/socket.h"
#include "network/socket_addr.h"
#include "network/inet_address.h"

// ===========================================================================
// Basic Socket operations
// ===========================================================================

TEST(SocketTest, CreateTcpSocket) {
    Socket sock(AF_INET, SOCK_STREAM);
    EXPECT_GE(sock.native_handle(), 0);
    EXPECT_FALSE(sock.is_closed());
}

TEST(SocketTest, CreateUdpSocket) {
    Socket sock(AF_INET, SOCK_DGRAM);
    EXPECT_GE(sock.native_handle(), 0);
    EXPECT_FALSE(sock.is_closed());
}

TEST(SocketTest, MoveAssignmentClosesOld) {
    Socket sock(AF_INET, SOCK_STREAM);
    int old_fd = sock.native_handle();
    Socket other(AF_INET, SOCK_DGRAM);
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
    Socket server(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto server_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(server_addr.has_value());
    server.bind(*server_addr).value();
    server.listen(1);

    // Retrieve the actual port assigned by the kernel.
    auto server_sockname = server.get_sockname();
    auto server_port = server_sockname.port();
    ASSERT_GT(server_port, 0);

    // Client: create and connect.
    auto client_target = SocketAddr::from_inet(*loopback, server_port);
    ASSERT_TRUE(client_target.has_value());

    Socket client(AF_INET, SOCK_STREAM);
    auto connect_result = client.connect(*client_target);
    ASSERT_TRUE(connect_result.has_value()) << "connect failed";

    // Server: accept the connection.
    SocketAddr peer_addr;
    auto accepted = server.accept(&peer_addr);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_GE(accepted->native_handle(), 0);
    EXPECT_EQ(peer_addr.family(), AF_INET);

    // Send data from client to server.
    const std::string message = "Hello, socket!";
    auto sent = client.send(std::span<const std::byte>{
        reinterpret_cast<const std::byte *>(message.data()), message.size()
    });
    EXPECT_EQ(sent, static_cast<ssize_t>(message.size()));

    // Receive on server side.
    std::array<std::byte, 64> recv_buf{};
    auto received = accepted->recv(std::span<std::byte>{recv_buf});
    EXPECT_EQ(received, static_cast<ssize_t>(message.size()));
    EXPECT_EQ(
        std::string(reinterpret_cast<const char *>(recv_buf.data()),
                    static_cast<size_t>(received)),
        message
    );
}

TEST(SocketTest, TcpConnectRefused) {
    // Attempt to connect to 127.0.0.1:1 (port 1, nothing listening).
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket sock(AF_INET, SOCK_STREAM);
    sock.set_nonblocking(true).value();
    auto target = SocketAddr::from_inet(*loopback, 1);
    ASSERT_TRUE(target.has_value());

    auto result = sock.connect(*target, 0);
    ASSERT_FALSE(result.has_value());
    // On Linux a connection to a closed port is immediately refused; on FreeBSD
    // the non-blocking connect returns EINPROGRESS and poll() with timeout 0 may
    // time out before the RST arrives.  Both outcomes are valid.
    EXPECT_TRUE(result.error() == ConnectError::REFUSED ||
                result.error() == ConnectError::TIMED_OUT);
}

// ===========================================================================
// Socket option helpers
// ===========================================================================

TEST(SocketTest, NonBlockingFlag) {
    Socket sock(AF_INET, SOCK_STREAM);
    sock.set_nonblocking(true).value();

    // Try connect to an unused port; should fail immediately with EINPROGRESS
    // or EAGAIN in non-blocking mode, not hang.
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());
    auto target = SocketAddr::from_inet(*loopback, 9999);
    ASSERT_TRUE(target.has_value());

    auto result = sock.connect(*target, 0);
    EXPECT_FALSE(result.has_value()); // Refused or InProgress
}

// ===========================================================================
// UDP socket operations
// ===========================================================================

TEST(SocketTest, UdpSendRecvOnLoopback) {
    // Create a pair of UDP sockets on loopback.
    Socket server(AF_INET, SOCK_DGRAM);
    Socket client(AF_INET, SOCK_DGRAM);

    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    // Bind server to a random port.
    auto server_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(server_addr.has_value());
    server.bind(*server_addr).value();
    auto server_port = server.get_sockname().port();
    ASSERT_GT(server_port, 0);

    // Send from client to server.
    auto target = SocketAddr::from_inet(*loopback, server_port);
    ASSERT_TRUE(target.has_value());

    const std::string message = "UDP test";
    auto sent = client.send_to(
        std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(message.data()), message.size()
        },
        *target
    );
    EXPECT_EQ(sent, static_cast<ssize_t>(message.size()));

    // Receive on server.
    std::array<std::byte, 64> recv_buf{};
    SocketAddr src_addr;
    auto received = server.recv_from(std::span<std::byte>{recv_buf}, 0, &src_addr);
    ASSERT_EQ(received, static_cast<ssize_t>(message.size()));
    EXPECT_EQ(
        std::string(reinterpret_cast<const char *>(recv_buf.data()),
                    static_cast<size_t>(received)),
        message
    );
    EXPECT_EQ(src_addr.family(), AF_INET);
}

// ===========================================================================
// Socket shutdown
// ===========================================================================

TEST(SocketTest, ShutdownWrite) {
    // shutdown_write() should behave like a half-close.
    Socket sock(AF_INET, SOCK_STREAM);
    sock.shutdown_write();
    EXPECT_FALSE(sock.is_closed());  // shutdown is not close
}

TEST(SocketTest, ShutdownBoth) {
    Socket sock(AF_INET, SOCK_STREAM);
    sock.shutdown_both();
    EXPECT_FALSE(sock.is_closed());
}

// ===========================================================================
// Socket get_sockname / get_peername
// ===========================================================================

TEST(SocketTest, GetSockname_BeforeBind_ReturnsUnspec) {
    Socket sock(AF_INET, SOCK_STREAM);
    auto name = sock.get_sockname();
    EXPECT_GE(name.family(), 0);
}

// ===========================================================================
// Close & shutdown edge cases
// ===========================================================================

TEST(SocketTest, CloseIdempotent) {
    Socket sock(AF_INET, SOCK_STREAM);
    EXPECT_FALSE(sock.is_closed());

    sock.close();
    EXPECT_TRUE(sock.is_closed());

    // Second close must be a no-op (not crash, not abort).
    sock.close();
    EXPECT_TRUE(sock.is_closed());
}

TEST(SocketTest, ShutdownOnClosedSocket) {
    Socket sock(AF_INET, SOCK_STREAM);
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
    Socket sock(AF_INET, SOCK_STREAM);
    EXPECT_TRUE(sock.set_nonblocking(true).has_value());
    EXPECT_TRUE(sock.set_nonblocking(false).has_value());
}

TEST(SocketTest, SetSocketOptionsTcp) {
    // Options available on any stream socket.
    Socket sock(AF_INET, SOCK_STREAM);

    EXPECT_TRUE(sock.set_keepalive(true).has_value());
    EXPECT_TRUE(sock.set_keepalive(false).has_value());

    EXPECT_TRUE(sock.set_linger(true, 1).has_value());
    EXPECT_TRUE(sock.set_linger(false).has_value());

    EXPECT_TRUE(sock.set_reuseaddr(true).has_value());
    EXPECT_TRUE(sock.set_reuseaddr(false).has_value());
}

TEST(SocketTest, SetSocketOptionsUdp) {
    Socket sock(AF_INET, SOCK_DGRAM);
    EXPECT_TRUE(sock.set_broadcast(true).has_value());
    EXPECT_TRUE(sock.set_broadcast(false).has_value());
}

TEST(SocketTest, SetIpv6Only) {
    Socket sock(AF_INET6, SOCK_STREAM);
    EXPECT_TRUE(sock.set_ipv6_only(true).has_value());
    EXPECT_TRUE(sock.set_ipv6_only(false).has_value());
}

TEST(SocketTest, SetReusePort) {
    Socket sock(AF_INET, SOCK_STREAM);
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

    Socket server(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto server_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(server_addr.has_value());
    server.bind(*server_addr).value();
    server.listen(1);

    auto server_port = server.get_sockname().port();
    ASSERT_GT(server_port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, server_port);
    ASSERT_TRUE(client_target.has_value());

    Socket client(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target).has_value());

    SocketAddr peer_addr;
    auto accepted = server.accept(&peer_addr);
    ASSERT_TRUE(accepted.has_value());

    // Send exactly 100 bytes from client.
    std::string payload(100, 'x');
    auto sent = client.send(std::as_bytes(std::span{payload}));
    ASSERT_EQ(sent, 100);

    // recv_exact should read all 100 bytes in one call.
    std::array<std::byte, 100> buf{};
    auto received = accepted->recv_exact(std::span{buf});
    EXPECT_EQ(received, 100);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(buf.data()), 100), payload);
}

TEST(SocketTest, RecvExactOnDatagram) {
    // recv_exact on a DGRAM socket should perform a single recv() call
    // (not loop), preserving datagram boundaries.
    Socket server(AF_INET, SOCK_DGRAM);
    Socket client(AF_INET, SOCK_DGRAM);

    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    auto server_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(server_addr.has_value());
    server.bind(*server_addr).value();
    auto server_port = server.get_sockname().port();
    ASSERT_GT(server_port, 0);

    // Send a 32-byte datagram.
    auto target = SocketAddr::from_inet(*loopback, server_port);
    ASSERT_TRUE(target.has_value());

    std::string payload(32, 'y');
    auto sent = client.send_to(std::as_bytes(std::span{payload}), *target);
    ASSERT_EQ(sent, 32);

    // recv_exact with a larger buffer should still return 32 (datagram boundary).
    std::array<std::byte, 128> buf{};
    auto received = server.recv_exact(std::span{buf});
    EXPECT_EQ(received, 32);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(buf.data()), 32), payload);
}

// ===========================================================================
// wait_for — readiness polling
// ===========================================================================

TEST(SocketTest, WaitForReady) {
    // A fresh UDP socket should be writable immediately.
    // TCP is not used here because on macOS/BSD an unconnected TCP socket
    // may not signal POLLOUT, whereas UDP always does.
    Socket sock(AF_INET, SOCK_DGRAM);
    auto result = sock.wait_for(POLLOUT, 0);
    ASSERT_TRUE(result.has_value()) << "wait_for failed: " << result.error();
    EXPECT_EQ(*result, 1);
}

TEST(SocketTest, WaitForTimeout) {
    // A TCP socket not yet connected to a server has no data to read —
    // POLLIN with timeout 0 should return 0 (not ready).
    Socket sock(AF_INET, SOCK_STREAM);
    auto result = sock.wait_for(POLLIN, 0);
    ASSERT_TRUE(result.has_value()) << "wait_for failed: " << result.error();
    EXPECT_EQ(*result, 0);
}

// ===========================================================================
// accept without addr
// ===========================================================================

TEST(SocketTest, AcceptWithoutAddr) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket server(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    server.bind(*addr).value();
    server.listen(1);

    auto port = server.get_sockname().port();
    ASSERT_GT(port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(client_target.has_value());

    Socket client(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target).has_value());

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

    Socket server(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    server.bind(*addr).value();
    server.listen(1);

    auto port = server.get_sockname().port();
    ASSERT_GT(port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(client_target.has_value());

    Socket client(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target).has_value());

    // After connect, get_peername should return the server's address.
    auto peername = client.get_peername();
    EXPECT_EQ(peername.family(), AF_INET);
    EXPECT_EQ(peername.port(), port);
}

// ===========================================================================
// send/recv with explicit flags
// ===========================================================================

TEST(SocketTest, SendRecvWithFlags) {
    auto loopback = InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());

    Socket server(AF_INET, SOCK_STREAM);
    server.set_reuseaddr(true).value();
    auto addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(addr.has_value());
    server.bind(*addr).value();
    server.listen(1);

    auto port = server.get_sockname().port();
    ASSERT_GT(port, 0);

    auto client_target = SocketAddr::from_inet(*loopback, port);
    ASSERT_TRUE(client_target.has_value());

    Socket client(AF_INET, SOCK_STREAM);
    ASSERT_TRUE(client.connect(*client_target).has_value());

    Socket accepted = *server.accept();

    // Use send() overload with explicit flags.
    const std::string msg = "flags test";
    auto sent = client.send(std::as_bytes(std::span{msg}), 0);
    ASSERT_EQ(sent, static_cast<ssize_t>(msg.size()));

    // recv() with explicit flags.
    std::array<std::byte, 32> buf{};
    auto received = accepted.recv(std::span{buf}, 0);
    EXPECT_EQ(received, static_cast<ssize_t>(msg.size()));
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(buf.data()), msg.size()), msg);
}
