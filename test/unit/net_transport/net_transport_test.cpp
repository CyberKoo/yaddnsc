//
// Unit tests for Transport (src/network/transport/).
//
// Verifies (no network I/O):
//   - Options field defaults.
//   - Eager host validation in TlsStream / TcpStream constructors.
//   - detail::poll_fd cancellation semantics using a silent pipe (the same
//     primitive every stream I/O path is built on).
// =============================================================================

#include <chrono>
#include <thread>

#include <arpa/inet.h>
#include <cerrno>
#include <expected>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "network/transport/detail/socket_stream.h"
#include "network/transport/io_error.h"
#include "network/transport/options.h"
#include "network/transport/tcp_stream.h"
#include "network/transport/tls_stream.h"
#include "util/cancellation_token.hpp"
#include "util/fd.hpp"

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

    const Transport::TlsOptions tls;
    EXPECT_TRUE(tls.verify_peer);
    EXPECT_FALSE(tls.sni_hostname.has_value());
    EXPECT_FALSE(tls.ca_bundle.has_value());
    EXPECT_TRUE(tls.alpn_proto.empty());
}

// ── Eager host validation ────────────────────────────────────────────────────

TEST(NetTransportCtor, TlsStream_RejectsInvalidHost) {
    const Utils::CancellationToken token;
    EXPECT_THROW((Transport::TlsStream("not_a_valid_address!!!", 443, {}, {}, token)), std::invalid_argument);
}

TEST(NetTransportCtor, TcpStream_RejectsInvalidHost) {
    const Utils::CancellationToken token;
    EXPECT_THROW((Transport::TcpStream("not_a_valid_address!!!", 80, {}, token)), std::invalid_argument);
}

TEST(NetTransportCtor, AcceptsIpLiteralAndDomain) {
    const Utils::CancellationToken token;
    EXPECT_NO_THROW((Transport::TlsStream("127.0.0.1", 443, {}, {}, token)));
    EXPECT_NO_THROW((Transport::TlsStream("dns.example.com", 443, {}, {}, token)));
    EXPECT_NO_THROW((Transport::TcpStream("::1", 80, {}, token)));
}

// ── poll_fd cancellation semantics ───────────────────────────────────────────

namespace {

/// A pipe whose read end never becomes ready (silent writer end kept open).
struct SilentFd {
    SilentFd() {
        auto [r, w] = Utils::make_pipe();
        read = std::move(r);
        write = std::move(w);
    }

    Utils::UniqueFd read;
    Utils::UniqueFd write;
};

}  // namespace

TEST(NetTransportPollFd, SilentFd_TimesOut) {
    const SilentFd silent;
    const Utils::CancellationToken token;

    const auto result = Transport::detail::poll_fd(silent.read.get(), POLLIN, 20ms, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::TIMEOUT);
}

TEST(NetTransportPollFd, TriggeredToken_ReturnsCancelledImmediately) {
    const SilentFd silent;
    Utils::CancellationSource source;
    source.trigger();
    const auto token = source.token();

    const auto result = Transport::detail::poll_fd(silent.read.get(), POLLIN, 5000ms, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
}

TEST(NetTransportPollFd, TriggerFromAnotherThread_WakesPoll) {
    const SilentFd silent;
    Utils::CancellationSource source;
    const auto token = source.token();

    std::jthread triggerrer([src = source] {
        std::this_thread::sleep_for(30ms);
        src.trigger();
    });

    const auto start = std::chrono::steady_clock::now();
    const auto result = Transport::detail::poll_fd(silent.read.get(), POLLIN, 5000ms, token);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
    EXPECT_LT(elapsed, 2s);
}

TEST(NetTransportPollFd, DrainedSignal_StillCancelledViaLatch) {
    const SilentFd silent;
    Utils::CancellationSource source;
    const auto token = source.token();

    source.trigger();
    token.drain();  // another consumer drains the pipe edge...

    // ...the latched flag must still cancel the operation.
    const auto result = Transport::detail::poll_fd(silent.read.get(), POLLIN, 5000ms, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
}

TEST(NetTransportPollFd, ReadyFd_ReturnsOk) {
    auto [read_end, write_end] = Utils::make_pipe();
    const Utils::CancellationToken token;

    const char c = 'x';
    ASSERT_EQ(::write(write_end.get(), &c, 1), 1);

    const auto result = Transport::detail::poll_fd(read_end.get(), POLLIN, 100ms, token);
    EXPECT_TRUE(result);
}

// ── error paths that need no connection (no network I/O) ─────────────────────

TEST(NetTransportErrorPaths, TcpStream_ReadSome_WithoutConnection_Fails) {
    const Utils::CancellationToken token;
    Transport::TcpStream stream("127.0.0.1", 80, {}, token);

    std::uint8_t buf[4];
    const auto result = stream.read_some(buf);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, TcpStream_SendAll_WithoutConnection_Fails) {
    const Utils::CancellationToken token;
    Transport::TcpStream stream("127.0.0.1", 80, {}, token);

    const std::uint8_t data[4] = {1, 2, 3, 4};
    const auto result = stream.send_all(data);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, TlsStream_ReadSome_WithoutHandshake_Fails) {
    const Utils::CancellationToken token;
    Transport::TlsStream stream("127.0.0.1", 443, {}, {}, token);

    std::uint8_t buf[4];
    const auto result = stream.read_some(buf);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, TlsStream_SendAll_WithoutHandshake_Fails) {
    const Utils::CancellationToken token;
    Transport::TlsStream stream("127.0.0.1", 443, {}, {}, token);

    const std::uint8_t data[4] = {1, 2, 3, 4};
    const auto result = stream.send_all(data);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, SocketStream_Poll_WithoutFd_Fails) {
    const Utils::CancellationToken token;
    const Transport::detail::SocketStream stream("127.0.0.1", 80, {}, token);

    const auto result = stream.poll(POLLIN, 0ms);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, SocketStream_Connect_PreTriggeredToken_Cancelled) {
    Utils::CancellationSource source;
    source.trigger();

    Transport::detail::SocketStream stream("127.0.0.1", 80, {}, source.token());
    const auto result = stream.connect();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
}

TEST(NetTransportErrorPaths, SocketStream_Connect_ZeroBudget_TimesOut) {
    const Utils::CancellationToken token;
    Transport::detail::SocketStream stream("127.0.0.1", 80, {.connect_timeout = 0ms}, token);

    const auto result = stream.connect();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::TIMEOUT);
}

TEST(NetTransportErrorPaths, SocketStream_Connect_BogusInterface_Fails) {
    const Utils::CancellationToken token;
    Transport::detail::SocketStream stream("127.0.0.1", 80, {.interface = std::string("bogus0")}, token);

    const auto result = stream.connect();
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

TEST(NetTransportErrorPaths, SocketStream_Connect_RefusedPort_Fails) {
    const Utils::CancellationToken token;
    Transport::detail::SocketStream stream("127.0.0.1", closed_loopback_port(), {}, token);

    const auto result = stream.connect();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportErrorPaths, SocketStream_Connect_UnresolvableHost_Fails) {
    const Utils::CancellationToken token;
    // Syntactically valid (passes eager validation), guaranteed non-existent.
    Transport::detail::SocketStream stream("no-such-host-yaddnsc.invalid", 443, {}, token);

    const auto result = stream.connect();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST(NetTransportPollFd, AsyncError_WithoutRequestedEvent_ReturnsConnectionFailed) {
    const Utils::CancellationToken token;

    // Non-blocking connect to a closed port: the refused-port RST surfaces
    // through poll as POLLERR. Passing events=0 keeps the readiness bitmask
    // from matching, so the error-flag branch of poll_fd is exercised
    // (connect_one asks for POLLOUT, which would mask it).
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ASSERT_NE(flags, -1);
    ASSERT_EQ(::fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(closed_loopback_port());
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), -1);
    ASSERT_EQ(errno, EINPROGRESS);

    const auto result = Transport::detail::poll_fd(fd, 0, 2s, token);
    ::close(fd);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}
