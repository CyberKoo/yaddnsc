//
// Unit tests for Transport (src/infrastructure/network/transport/).
//
// Verifies (no byte-stream I/O):
//   - Options field defaults.
//   - Eager host validation in TlsStream / TcpStream constructors.
//   - TcpConnection connect() rejection before a handshake: cancellation,
//     an already-due deadline, a missing bootstrap server, a bad interface.
// Readiness waiting is covered on Socket in the component socket tests.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <expected>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "infrastructure/network/transport/detail/tcp_connection.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "infrastructure/network/transport/tls_stream.h"
#include "support/util/cancellation_token.hpp"

using namespace std::chrono_literals;
using Transport::IoError;

// ── Options defaults ─────────────────────────────────────────────────────────

TEST(NetTransportOptions, Defaults) {
    const Transport::Options opts;
    EXPECT_EQ(opts.connect_timeout, 5000ms);
    EXPECT_EQ(opts.read_timeout, 5000ms);
    EXPECT_EQ(opts.write_timeout, 5000ms);
    EXPECT_FALSE(opts.interface.has_value());
    EXPECT_FALSE(opts.address_family.has_value());
    EXPECT_TRUE(opts.bootstrap_dns.empty());

    const Transport::TlsOptions tls;
    EXPECT_TRUE(tls.verify_peer);
    EXPECT_FALSE(tls.sni_hostname.has_value());
    EXPECT_FALSE(tls.ca_bundle.has_value());
    EXPECT_TRUE(tls.alpn_proto.empty());
}

// ── Eager host validation ────────────────────────────────────────────────────

TEST(NetTransportCtor, TlsStream_RejectsInvalidHost) {
    EXPECT_THROW((Transport::TlsStream("not_a_valid_address!!!", 443, {}, {})), std::invalid_argument);
}

TEST(NetTransportCtor, TcpStream_RejectsInvalidHost) {
    EXPECT_THROW((Transport::TcpStream("not_a_valid_address!!!", 80, {})), std::invalid_argument);
}

TEST(NetTransportCtor, AcceptsIpLiteralAndDomain) {
    EXPECT_NO_THROW((Transport::TlsStream("127.0.0.1", 443, {}, {})));
    EXPECT_NO_THROW((Transport::TlsStream("dns.example.com", 443, {}, {})));
    EXPECT_NO_THROW((Transport::TcpStream("::1", 80, {})));
}

// ── error paths that need no connection (no network I/O) ─────────────────────

TEST(NetTransportErrorPaths, TcpStream_ReadSome_WithoutConnection_Fails) {
    const Utils::CancellationToken token;
    Transport::TcpStream stream("127.0.0.1", 80, {});

    std::uint8_t buf[4];
    const auto result = stream.read_some(buf, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, TcpStream_SendAll_WithoutConnection_Fails) {
    const Utils::CancellationToken token;
    Transport::TcpStream stream("127.0.0.1", 80, {});

    const std::uint8_t data[4] = {1, 2, 3, 4};
    const auto result = stream.send_all(data, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

// close() is safe on an unconnected stream and is idempotent.
TEST(NetTransportErrorPaths, TcpStream_Close_WithoutConnection_IsNoOp) {
    Transport::TcpStream stream("127.0.0.1", 80, {});
    EXPECT_NO_THROW(stream.close());
    EXPECT_NO_THROW(stream.close());
}

TEST(NetTransportErrorPaths, TlsStream_ReadSome_WithoutHandshake_Fails) {
    const Utils::CancellationToken token;
    Transport::TlsStream stream("127.0.0.1", 443, {}, {});

    std::uint8_t buf[4];
    const auto result = stream.read_some(buf, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, TlsStream_SendAll_WithoutHandshake_Fails) {
    const Utils::CancellationToken token;
    Transport::TlsStream stream("127.0.0.1", 443, {}, {});

    const std::uint8_t data[4] = {1, 2, 3, 4};
    const auto result = stream.send_all(data, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, TcpConnection_Unconnected_IsNotHealthy) {
    const Transport::detail::TcpConnection connection("127.0.0.1", 80, {});
    EXPECT_FALSE(connection.is_connected());
    EXPECT_FALSE(connection.is_healthy());
}

TEST(NetTransportErrorPaths, TcpConnection_Connect_PreTriggeredToken_Cancelled) {
    Utils::CancellationSource source;
    source.trigger();

    Transport::detail::TcpConnection connection("127.0.0.1", 80, {});
    const auto result = connection.connect(std::chrono::steady_clock::now() + 2s, source.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
}

TEST(NetTransportErrorPaths, TcpConnection_Connect_ZeroBudget_TimesOut) {
    const Utils::CancellationToken token;
    // The deadline is the caller's. connect_timeout on Options is not read.
    Transport::detail::TcpConnection connection("127.0.0.1", 80, {.connect_timeout = 5s});

    const auto result = connection.connect(std::chrono::steady_clock::now(), token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::TIMEOUT);
}

TEST(NetTransportErrorPaths, TcpConnection_Connect_BogusInterface_Fails) {
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", 80, {.interface = std::string("bogus0")});

    const auto result = connection.connect(std::chrono::steady_clock::now() + 2s, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

namespace {

/// An ephemeral loopback port that is guaranteed to have no listener.
[[nodiscard]] std::uint16_t closed_loopback_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 1;  // fall back to a port that is virtually always closed
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 1;
    }
    const auto port = ntohs(addr.sin_port);
    ::close(fd);  // releasing the bound port leaves it closed
    return port;
}

}  // namespace

TEST(NetTransportErrorPaths, TcpConnection_Connect_RefusedPort_Fails) {
    const Utils::CancellationToken token;
    Transport::detail::TcpConnection connection("127.0.0.1", closed_loopback_port(), {});

    const auto result = connection.connect(std::chrono::steady_clock::now() + 2s, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, TcpConnection_Connect_UnresolvableHost_Fails) {
    const Utils::CancellationToken token;
    // Syntactically valid (passes eager validation), guaranteed non-existent.
    Transport::detail::TcpConnection connection("no-such-host-yaddnsc.invalid", 443, {});

    const auto result = connection.connect(std::chrono::steady_clock::now() + 2s, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, TcpConnection_Connect_HostnameWithoutBootstrap_FailsFast) {
    const Utils::CancellationToken token;
    // No bootstrap DNS servers configured: a hostname target must fail
    // immediately (no getaddrinfo fallback, no NSS lookup).
    Transport::detail::TcpConnection connection("example.com", 443, {});

    const auto start = std::chrono::steady_clock::now();
    const auto result = connection.connect(std::chrono::steady_clock::now() + 2s, token);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
    EXPECT_LT(elapsed, 1s);
}
