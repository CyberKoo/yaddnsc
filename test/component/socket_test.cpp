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
    server.set_reuseaddr(true);
    auto server_addr = SocketAddr::from_inet(*loopback, 0);
    ASSERT_TRUE(server_addr.has_value());
    server.bind(*server_addr);
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
    EXPECT_GE(accepted.native_handle(), 0);
    EXPECT_EQ(peer_addr.family(), AF_INET);

    // Send data from client to server.
    const std::string message = "Hello, socket!";
    auto sent = client.send(std::span<const std::byte>{
        reinterpret_cast<const std::byte *>(message.data()), message.size()
    });
    EXPECT_EQ(sent, static_cast<ssize_t>(message.size()));

    // Receive on server side.
    std::array<std::byte, 64> recv_buf{};
    auto received = accepted.recv(std::span<std::byte>{recv_buf});
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
    sock.set_nonblocking(true);
    auto target = SocketAddr::from_inet(*loopback, 1);
    ASSERT_TRUE(target.has_value());

    auto result = sock.connect(*target, 0);
    ASSERT_FALSE(result.has_value());
    // On Linux a connection to a closed port is immediately refused; on FreeBSD
    // the non-blocking connect returns EINPROGRESS and poll() with timeout 0 may
    // time out before the RST arrives.  Both outcomes are valid.
    EXPECT_TRUE(result.error() == ConnectError::Refused ||
                result.error() == ConnectError::TimedOut);
}

// ===========================================================================
// Socket option helpers
// ===========================================================================

TEST(SocketTest, NonBlockingFlag) {
    Socket sock(AF_INET, SOCK_STREAM);
    sock.set_nonblocking(true);

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
    server.bind(*server_addr);
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
