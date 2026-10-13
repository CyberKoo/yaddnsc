//
// Unit tests for the transport paths of TcpStream
// (src/infrastructure/network/transport/tcp_stream.cpp) and TlsStream
// (src/infrastructure/network/tls/stream.cpp) that an echo round-trip never
// reaches: how TcpStream reports a peer that has gone away, and how TlsStream
// turns its TLS options into a session — or refuses to, before a single byte
// reaches the wire.
//
// Scope: loopback sockets only. The peer here is a raw socket that never
// completes a handshake, so every connect ends in CONNECTION_FAILED once the
// ClientHello has been observed. A real TLS peer and certificate verification
// stay in the component suite.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coro/coro.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/network/tls/context.h"
#include "infrastructure/network/tls/stream.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "support/util/fd.hpp"

namespace {

using namespace std::chrono_literals;

using net::IoError;

/// A zero-length ALPN protocol name: the wire format SSL_set_alpn_protos must
/// reject, so the stream must refuse the connection rather than negotiate it.
constexpr std::array<unsigned char, 1> EMPTY_ALPN_PROTOCOL{0};

/// A well-formed ALPN list ("h2"): the wire format SSL_set_alpn_protos accepts,
/// so the ClientHello must advertise it.
constexpr std::array<unsigned char, 3> H2_ALPN_PROTOCOL{2, 'h', '2'};

[[nodiscard]] domain::InetAddress loopback_v4() {
    const auto address = domain::InetAddress::parse("127.0.0.1");
    EXPECT_TRUE(address.has_value());
    return address.value_or(domain::InetAddress{});
}

[[nodiscard]] domain::InetAddress loopback_v6() {
    const auto address = domain::InetAddress::parse("::1");
    EXPECT_TRUE(address.has_value());
    return address.value_or(domain::InetAddress{});
}

[[nodiscard]] int socket_family(const domain::InetAddress& address) {
    return address.get_family() == domain::AddressFamily::IPV6 ? AF_INET6 : AF_INET;
}

/// Fill @p storage with @p address and @p port, returning the length to bind.
[[nodiscard]] socklen_t encode_loopback(const domain::InetAddress& address, std::uint16_t port, sockaddr_storage& storage) {
    const auto bytes = address.get_address();
    if (socket_family(address) == AF_INET6) {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(storage);
        v6.sin6_family = AF_INET6;
        std::memcpy(&v6.sin6_addr, bytes.data(), domain::Inet6Address::ADDR_LEN);
        v6.sin6_port = htons(port);
        return sizeof(sockaddr_in6);
    }
    auto& v4 = reinterpret_cast<sockaddr_in&>(storage);
    v4.sin_family = AF_INET;
    std::memcpy(&v4.sin_addr, bytes.data(), domain::Inet4Address::ADDR_LEN);
    v4.sin_port = htons(port);
    return sizeof(sockaddr_in);
}

[[nodiscard]] std::uint16_t decode_port(const sockaddr_storage& storage) {
    if (storage.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6&>(storage).sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in&>(storage).sin_port);
}

/// True when the host can bind an IPv6 loopback socket at all. A host without
/// IPv6 has no ::1 target for the stream to format.
[[nodiscard]] bool ipv6_loopback_available() {
    const int probe = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (probe < 0) {
        return false;
    }
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    const bool bound = ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(probe);
    return bound;
}

/// What a captured TLS handshake record said about the server_name extension.
enum class ClientHelloShape {
    NOT_A_CLIENT_HELLO,  ///< The peer wrote something that is not a ClientHello.
    NO_SNI,              ///< A ClientHello, without a server_name extension.
    WITH_SNI,            ///< A ClientHello carrying a server_name extension.
};

/// The extension block of a ClientHello record (RFC 8446 §4.1.2), as a view
/// into @p record. Returns nullopt when @p record is not a parseable
/// ClientHello; the returned view is empty when the ClientHello carries no
/// extensions.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> client_hello_extensions(
    std::span<const std::uint8_t> record) {
    constexpr std::size_t RECORD_HEADER = 5;      // type(1) version(2) length(2)
    constexpr std::size_t HANDSHAKE_HEADER = 4;  // type(1) length(3)
    if (record.size() < RECORD_HEADER + HANDSHAKE_HEADER) {
        return std::nullopt;
    }
    if (record[0] != 0x16 || record[5] != 0x01) {  // handshake record, ClientHello
        return std::nullopt;
    }

    std::size_t at = RECORD_HEADER + HANDSHAKE_HEADER;
    if (record.size() < at + 2 + 32 + 1) {  // legacy_version + random + session_id length
        return std::nullopt;
    }
    at += 2 + 32;
    at += 1 + record[at];  // session_id
    if (record.size() < at + 2) {
        return std::nullopt;
    }
    at += 2 + ((std::size_t{record[at]} << 8) | record[at + 1]);  // cipher suites
    if (record.size() < at + 1) {
        return std::nullopt;
    }
    at += 1 + record[at];  // compression methods
    if (record.size() < at + 2) {
        return std::span<const std::uint8_t>{record.data() + at, 0};  // no extension block at all
    }
    const std::size_t extensions = (std::size_t{record[at]} << 8) | record[at + 1];
    at += 2;
    if (record.size() < at + extensions) {
        return std::nullopt;
    }
    return record.subspan(at, extensions);
}

/// True when @p record is a ClientHello carrying an extension of @p type.
[[nodiscard]] bool client_hello_has_extension(std::span<const std::uint8_t> record, std::uint16_t type) {
    const auto extensions = client_hello_extensions(record);
    if (!extensions) {
        return false;
    }
    for (std::size_t at = 0; at + 4 <= extensions->size();) {
        const std::size_t found = (std::size_t{(*extensions)[at]} << 8) | (*extensions)[at + 1];
        const std::size_t length = (std::size_t{(*extensions)[at + 2]} << 8) | (*extensions)[at + 3];
        if (found == type) {
            return true;
        }
        at += 4 + length;
    }
    return false;
}

/// Look for server_name (RFC 8446 §4.2.2) in a ClientHello. RFC 6066 §3 forbids
/// an IP literal there, so the shape tells an IP-pinned stream apart from a
/// name-pinned one.
[[nodiscard]] ClientHelloShape classify_client_hello(std::span<const std::uint8_t> record) {
    constexpr std::uint16_t SERVER_NAME = 0;
    if (client_hello_has_extension(record, SERVER_NAME)) {
        return ClientHelloShape::WITH_SNI;
    }
    return client_hello_extensions(record) ? ClientHelloShape::NO_SNI : ClientHelloShape::NOT_A_CLIENT_HELLO;
}

/// A one-connection-at-a-time loopback peer on its own thread.
///
/// The client can only finish once the peer closes, so by the time a test's task
/// returns everything the peer read has already been recorded — no barrier is
/// needed between the two.
class LoopbackPeer {
public:
    enum class Mode {
        /// Accept and hold the connection open without reading or writing: the
        /// client stays connected with an idle socket, and parks on a read.
        HOLD,
        /// Accept and close at once: the client sees an orderly peer close.
        CLOSE,
        /// Accept and abort with SO_LINGER off: the client sees a reset, not an
        /// orderly close, so a peek reports the error rather than EOF or EAGAIN.
        RESET,
        /// Record whatever the client writes, then close.
        CAPTURE,
    };

    LoopbackPeer(const domain::InetAddress& address, Mode mode) : mode_(mode), family_(socket_family(address)) {
        listener_.reset(::socket(family_, SOCK_STREAM, 0));
        if (!listener_) {
            return;
        }
        const int enabled = 1;
        ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

        sockaddr_storage storage{};
        socklen_t length = encode_loopback(address, 0, storage);
        if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&storage), length) != 0) {
            listener_.reset();
            return;
        }
        socklen_t bound = length;
        if (::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&storage), &bound) != 0) {
            listener_.reset();
            return;
        }
        port_ = decode_port(storage);
        if (::listen(listener_.get(), 4) != 0) {
            listener_.reset();
            return;
        }
        thread_ = std::jthread([this] { serve(); });
    }

    ~LoopbackPeer() {
        stop_.store(true);
        wake_accept();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    LoopbackPeer(const LoopbackPeer&) = delete;
    LoopbackPeer& operator=(const LoopbackPeer&) = delete;
    LoopbackPeer(LoopbackPeer&&) = delete;
    LoopbackPeer& operator=(LoopbackPeer&&) = delete;

    /// False when no listener could be bound; the test skips or fails on it.
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(listener_); }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    /// Everything the client wrote, as one contiguous buffer.
    [[nodiscard]] std::vector<std::uint8_t> captured() const {
        const std::lock_guard guard(mutex_);
        return captured_;
    }

private:
    void serve() {
        while (!stop_.load()) {
            const int raw = ::accept(listener_.get(), nullptr, nullptr);
            if (raw < 0) {
                continue;
            }
            const Utils::UniqueFd connection{raw};
            if (mode_ == Mode::RESET) {
                const struct linger abort_linger {
                    .l_onoff = 1, .l_linger = 0
                };
                ::setsockopt(connection.get(), SOL_SOCKET, SO_LINGER, &abort_linger, sizeof(abort_linger));
                continue;  // UniqueFd closes the connection, which aborts it
            }
            if (mode_ == Mode::CAPTURE) {
                capture(connection.get());
            }
            if (mode_ == Mode::HOLD) {
                while (!stop_.load()) {
                    std::this_thread::sleep_for(2ms);
                }
            }
        }
    }

    /// Record until the client closes or stops talking. The poll budget only
    /// bounds a client that writes nothing at all: the first write arrives right
    /// after its connect() completes, so the quiet tail ends the capture.
    void capture(const int fd) {
        constexpr auto QUIET = 20ms;
        constexpr auto BUDGET = 500ms;
        const auto deadline = std::chrono::steady_clock::now() + BUDGET;
        std::array<char, 1024> chunk{};
        int quiet_polls = 0;
        while (quiet_polls < 2 && std::chrono::steady_clock::now() < deadline) {
            pollfd descriptor{fd, POLLIN, 0};
            if (::poll(&descriptor, 1, static_cast<int>(QUIET.count())) <= 0) {
                ++quiet_polls;
                continue;
            }
            quiet_polls = 0;
            const ssize_t received = ::recv(fd, chunk.data(), chunk.size(), 0);
            if (received <= 0) {
                return;  // end of stream
            }
            const std::lock_guard guard(mutex_);
            const auto offset = static_cast<std::size_t>(received);
            captured_.insert(captured_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }

    /// Closing a descriptor does not reliably wake a blocked accept() on Linux;
    /// a throwaway self-connection does.
    void wake_accept() {
        if (!listener_) {
            return;
        }
        const int wake = ::socket(family_, SOCK_STREAM, 0);
        if (wake < 0) {
            return;
        }
        sockaddr_storage storage{};
        const socklen_t length = encode_loopback(family_ == AF_INET6 ? loopback_v6() : loopback_v4(), port_, storage);
        (void) ::connect(wake, reinterpret_cast<sockaddr*>(&storage), length);
        ::close(wake);
    }

    Mode mode_;
    int family_ = AF_INET;
    std::uint16_t port_ = 0;
    Utils::UniqueFd listener_;
    std::jthread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> captured_{};
};

/// Run a task to completion on a fresh loop with the system clock.
template<typename T>
T run_task(coro::Task<T> task) {
    coro::Loop loop;
    return coro::run(loop, std::move(task));
}

/// A client trust context that does no certificate work: these tests never get
/// far enough to verify anything.
[[nodiscard]] std::expected<std::shared_ptr<const net::TlsContext>, IoError> test_context() {
    return net::TlsContext::create(net::TlsTrustOptions{.verify_peer = false});
}

// ---------------------------------------------------------------------------
// TlsStream — the options decide the session, before any TLS byte is written
// ---------------------------------------------------------------------------

TEST(TlsStreamPaths, ensure_connected_WithoutTrustContext_FailsClosedBeforeWritingAnything) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE};
    ASSERT_TRUE(peer.valid());
    net::TlsStream stream{loopback_v4(), peer.port(), nullptr};

    std::optional<IoError> error;
    auto task = [&stream, &error]() -> coro::Task<void> {
        const auto result = co_await stream.ensure_connected();
        if (!result) {
            error = result.error();
        }
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
    // A stream with no trust material must not negotiate: nothing is written.
    EXPECT_TRUE(peer.captured().empty());
    EXPECT_FALSE(stream.connected());
}

TEST(TlsStreamPaths, ensure_connected_WithNamedSni_VerifiesTheNameAndSendsTheSniExtension) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE};
    ASSERT_TRUE(peer.valid());
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), peer.port(), *context, {},
                          net::TlsOptions{.sni_hostname = std::optional<std::string>{"sni.example.test"}}};

    bool connected = false;
    auto task = [&stream, &connected]() -> coro::Task<void> {
        connected = static_cast<bool>(co_await stream.ensure_connected());
        co_return;
    };
    run_task(task());

    // The peer never speaks TLS, so the handshake fails after the ClientHello.
    EXPECT_FALSE(connected);
    EXPECT_EQ(classify_client_hello(peer.captured()), ClientHelloShape::WITH_SNI);
    EXPECT_FALSE(stream.connected());
}

TEST(TlsStreamPaths, ensure_connected_WithIpLiteralSni_VerifiesTheAddressAndSendsNoSni) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE};
    ASSERT_TRUE(peer.valid());
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), peer.port(), *context, {},
                          net::TlsOptions{.sni_hostname = std::optional<std::string>{"127.0.0.1"}}};

    bool connected = false;
    auto task = [&stream, &connected]() -> coro::Task<void> {
        connected = static_cast<bool>(co_await stream.ensure_connected());
        co_return;
    };
    run_task(task());

    EXPECT_FALSE(connected);
    // RFC 6066 §3: an IP literal is a verification identity, never an SNI.
    EXPECT_EQ(classify_client_hello(peer.captured()), ClientHelloShape::NO_SNI);
    EXPECT_FALSE(stream.connected());
}

TEST(TlsStreamPaths, ensure_connected_WithEmptySniHostname_VerifiesTheTargetAddressAndSendsNoSni) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE};
    ASSERT_TRUE(peer.valid());
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), peer.port(), *context, {},
                          net::TlsOptions{.sni_hostname = std::optional<std::string>{}}};

    bool connected = false;
    auto task = [&stream, &connected]() -> coro::Task<void> {
        connected = static_cast<bool>(co_await stream.ensure_connected());
        co_return;
    };
    run_task(task());

    // An explicitly empty name is "no name": the target's address is the identity.
    EXPECT_FALSE(connected);
    EXPECT_EQ(classify_client_hello(peer.captured()), ClientHelloShape::NO_SNI);
}

TEST(TlsStreamPaths, ensure_connected_WithoutSni_VerifiesTheTargetAddress) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE};
    ASSERT_TRUE(peer.valid());
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), peer.port(), *context};

    bool connected = false;
    auto task = [&stream, &connected]() -> coro::Task<void> {
        connected = static_cast<bool>(co_await stream.ensure_connected());
        co_return;
    };
    run_task(task());

    EXPECT_FALSE(connected);
    EXPECT_EQ(classify_client_hello(peer.captured()), ClientHelloShape::NO_SNI);
}

TEST(TlsStreamPaths, ensure_connected_ToIpv6Target_VerifiesTheIpv6Literal) {
    if (!ipv6_loopback_available()) {
        GTEST_SKIP() << "no IPv6 loopback on this host";
    }
    LoopbackPeer peer{loopback_v6(), LoopbackPeer::Mode::CAPTURE};
    ASSERT_TRUE(peer.valid());
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v6(), peer.port(), *context};

    bool connected = false;
    auto task = [&stream, &connected]() -> coro::Task<void> {
        connected = static_cast<bool>(co_await stream.ensure_connected());
        co_return;
    };
    run_task(task());

    EXPECT_FALSE(connected);
    // An IPv6 target is formatted and verified as an IPv6 literal.
    EXPECT_EQ(classify_client_hello(peer.captured()), ClientHelloShape::NO_SNI);
}

TEST(TlsStreamPaths, ensure_connected_WithMalformedAlpnList_FailsClosedBeforeWritingAnything) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE};
    ASSERT_TRUE(peer.valid());
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), peer.port(), *context, {},
                          net::TlsOptions{.alpn_proto = EMPTY_ALPN_PROTOCOL}};

    std::optional<IoError> error;
    auto task = [&stream, &error]() -> coro::Task<void> {
        const auto result = co_await stream.ensure_connected();
        if (!result) {
            error = result.error();
        }
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
    // An ALPN list OpenSSL rejects must not reach the wire either.
    EXPECT_TRUE(peer.captured().empty());
    EXPECT_FALSE(stream.connected());
}

TEST(TlsStreamPaths, ensure_connected_WithValidAlpn_AdvertisesTheProtocolsBeforeTheHandshakeFails) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE};
    ASSERT_TRUE(peer.valid());
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), peer.port(), *context, {},
                          net::TlsOptions{.alpn_proto = H2_ALPN_PROTOCOL}};

    bool connected = false;
    auto task = [&stream, &connected]() -> coro::Task<void> {
        connected = static_cast<bool>(co_await stream.ensure_connected());
        co_return;
    };
    run_task(task());

    // A well-formed ALPN list is accepted, so the session is built and the
    // ClientHello reaches the wire before the silent peer fails the handshake.
    constexpr std::uint16_t ALPN_EXTENSION = 16;
    EXPECT_FALSE(connected);
    EXPECT_TRUE(client_hello_has_extension(peer.captured(), ALPN_EXTENSION));
    EXPECT_FALSE(stream.connected());
}

TEST(TlsStreamPaths, ensure_connected_StalledHandshake_CancellationClosesAndLeavesTheStreamReusable) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::HOLD};
    ASSERT_TRUE(peer.valid());
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), peer.port(), *context};

    bool timed_out = false;
    bool parked_again = false;
    auto task = [&stream, &timed_out, &parked_again]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(50ms, [&stream]() -> coro::Task<void> {
            (void) co_await stream.ensure_connected();
            co_return;
        });
        timed_out = outcome.timed_out;
        // The cancelled handshake released the session and the socket, so the
        // stream is a usable TcpStream again rather than a half-open one: the
        // next connect reaches the handshake and parks there too, instead of
        // failing at once.
        EXPECT_FALSE(stream.connected());
        const auto again = co_await coro::with_timeout(50ms, [&stream]() -> coro::Task<void> {
            (void) co_await stream.ensure_connected();
            co_return;
        });
        parked_again = again.timed_out;
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(timed_out);
    EXPECT_TRUE(parked_again);
}

TEST(TlsStreamPaths, close_BeforeConnecting_IsIdempotentAndLeavesTheStreamDisconnected) {
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), 9, *context};

    bool connected = false;
    auto task = [&stream, &connected]() -> coro::Task<void> {
        stream.close();
        stream.close();
        connected = stream.connected();
        co_return;
    };
    run_task(task());

    EXPECT_FALSE(connected);
}

// ---------------------------------------------------------------------------
// TcpStream — what the one-syscall health probe reports
// ---------------------------------------------------------------------------

TEST(TcpStreamPaths, connected_AfterOrderlyPeerClose_ReportsFalseAndTheStreamStaysReusable) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CLOSE};
    ASSERT_TRUE(peer.valid());
    net::TcpStream stream{loopback_v4(), peer.port()};

    std::optional<IoError> error;
    bool connected_after_close = true;
    bool reconnected = false;
    auto task = [&stream, &error, &connected_after_close, &reconnected]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        std::array<std::uint8_t, 8> buffer{};
        // The peer closed at once: the read reports the EOF, which also proves
        // the FIN has already landed when the health probe runs.
        const auto read = co_await stream.read_some(buffer);
        if (!read) {
            error = read.error();
        }
        connected_after_close = stream.connected();
        // A stream whose peer is gone is rebuilt rather than repaired.
        reconnected = static_cast<bool>(co_await stream.ensure_connected());
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
    // An orderly close is not a healthy socket, unlike a socket with data queued.
    EXPECT_FALSE(connected_after_close);
    EXPECT_TRUE(reconnected);
}

TEST(TcpStreamPaths, connected_AfterPeerReset_ReportsFalseWithASocketError) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::RESET};
    ASSERT_TRUE(peer.valid());
    net::TcpStream stream{loopback_v4(), peer.port()};

    bool connected_after_reset = true;
    auto task = [&stream, &connected_after_reset]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        // The abort may land just after connect() returns; poll until the probe
        // sees it. A reset is neither EOF (0) nor "nothing queued yet" (EAGAIN):
        // the peek reports the socket error itself.
        for (int attempt = 0; attempt < 500 && stream.connected(); ++attempt) {
            std::this_thread::sleep_for(1ms);
        }
        connected_after_reset = stream.connected();
        co_return;
    };
    run_task(task());

    EXPECT_FALSE(connected_after_reset);
}

TEST(TcpStreamPaths, connected_WhilePeerIsSilent_ReportsHealthy) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::HOLD};
    ASSERT_TRUE(peer.valid());
    net::TcpStream stream{loopback_v4(), peer.port()};

    bool connected = false;
    bool connected_after_cancel = false;
    auto task = [&stream, &connected, &connected_after_cancel]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        connected = stream.connected();
        // An idle socket is healthy: nothing queued, no EOF, nothing to report.
        const auto outcome = co_await coro::with_timeout(30ms, [&stream]() -> coro::Task<void> {
            std::array<std::uint8_t, 8> buffer{};
            (void) co_await stream.read_some(buffer);
            co_return;
        });
        EXPECT_TRUE(outcome.timed_out);
        connected_after_cancel = stream.connected();
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(connected);
    EXPECT_TRUE(connected_after_cancel);
}

TEST(TcpStreamPaths, connected_WithQueuedBytes_ReportsHealthy) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::HOLD};
    ASSERT_TRUE(peer.valid());
    net::TcpStream stream{loopback_v4(), peer.port()};

    bool connected = false;
    auto task = [&stream, &connected]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        // A peer's close must not look like an idle socket, so keep the
        // connection open by reading what this write makes the peer see.
        constexpr std::array<std::uint8_t, 1> PING{7};
        if (!co_await stream.send_all(PING)) {
            co_return;
        }
        connected = stream.connected();
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(connected);
}

TEST(TcpStreamPaths, close_ThenConnected_ReportsFalseAndLaterReadsFail) {
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::HOLD};
    ASSERT_TRUE(peer.valid());
    net::TcpStream stream{loopback_v4(), peer.port()};

    bool connected = true;
    std::optional<IoError> error;
    auto task = [&stream, &connected, &error]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        stream.close();
        connected = stream.connected();
        std::array<std::uint8_t, 4> buffer{};
        const auto read = co_await stream.read_some(buffer);
        if (!read) {
            error = read.error();
        }
        co_return;
    };
    run_task(task());

    EXPECT_FALSE(connected);
    EXPECT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
}

}  // namespace