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
#include <compare>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
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

enum class HelloFrame { INCOMPLETE, INVALID, COMPLETE };

/// TCP chunks and TLS records need not coincide. Only complete handshake
/// records containing the entire declared ClientHello count as completion.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, HelloFrame> reassemble_client_hello(
    std::span<const std::uint8_t> wire) {
    std::vector<std::uint8_t> handshake;
    for (std::size_t at = 0; at < wire.size();) {
        if (wire.size() - at < 5) {
            return std::unexpected(HelloFrame::INCOMPLETE);
        }
        const std::size_t length = (std::size_t{wire[at + 3]} << 8) | wire[at + 4];
        if (wire[at] != 0x16 || wire[at + 1] != 3 || length == 0 || length > 16384) {
            return std::unexpected(HelloFrame::INVALID);
        }
        at += 5;
        if (wire.size() - at < length) {
            return std::unexpected(HelloFrame::INCOMPLETE);
        }
        handshake.insert(handshake.end(), wire.begin() + static_cast<std::ptrdiff_t>(at),
                         wire.begin() + static_cast<std::ptrdiff_t>(at + length));
        if (handshake.size() >= 4) {
            if (handshake[0] != 1) {
                return std::unexpected(HelloFrame::INVALID);
            }
            const std::size_t expected =
                4 + ((std::size_t{handshake[1]} << 16) | (std::size_t{handshake[2]} << 8) | handshake[3]);
            if (handshake.size() >= expected) {
                handshake.resize(expected);
                return handshake;
            }
        }
        at += length;
    }
    return std::unexpected(HelloFrame::INCOMPLETE);
}

[[nodiscard]] HelloFrame client_hello_frame(std::span<const std::uint8_t> wire) {
    const auto handshake = reassemble_client_hello(wire);
    return handshake ? HelloFrame::COMPLETE : handshake.error();
}

/// Own the extension bytes: neither the reassembly nor the caller's wire buffer
/// needs to outlive this result. An empty vector means no extensions.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> client_hello_extensions(std::span<const std::uint8_t> wire) {
    const auto handshake = reassemble_client_hello(wire);
    if (!handshake) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> record{*handshake};
    std::size_t at = 4;                     // handshake type(1) length(3), no TLS record headers
    if (record.size() < at + 2 + 32 + 1) {  // legacy_version + random + session_id length
        return std::nullopt;
    }
    at += 2 + 32;
    at += 1u + record[at];  // session_id
    if (record.size() < at + 2) {
        return std::nullopt;
    }
    at += 2 + ((std::size_t{record[at]} << 8) | record[at + 1]);  // cipher suites
    if (record.size() < at + 1) {
        return std::nullopt;
    }
    at += 1u + record[at];  // compression methods
    if (at == record.size()) {
        return std::vector<std::uint8_t>{};  // no extension block at all
    }
    if (record.size() < at + 2) {
        return std::nullopt;
    }
    const std::size_t extensions = (std::size_t{record[at]} << 8) | record[at + 1];
    at += 2;
    if (record.size() < at + extensions) {
        return std::nullopt;
    }
    const auto bytes = record.subspan(at, extensions);
    return std::vector<std::uint8_t>{bytes.begin(), bytes.end()};
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
        if (length > extensions->size() - at - 4) {
            return false;
        }
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
        /// Accept, wait for the client's first byte, then abort with SO_LINGER
        /// off: the client sees a reset, not an orderly close, so the probe
        /// reports the pending error rather than EOF or EAGAIN. Waiting for
        /// the byte keeps the abort from racing the client's connect.
        RESET,
        /// Close only after a complete ClientHello (or EOF / malformed input).
        CAPTURE,
        /// Observe a complete ClientHello, then read until cancellation closes it.
        CAPTURE_HOLD,
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

    ~LoopbackPeer() { stop(); }

    /// Teardown is independent of protocol completion, including partial/no input.
    void stop() {
        if (!stop_.exchange(true)) {
            wake_accept();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] int accepted() const noexcept { return accepted_.load(); }

    [[nodiscard]] int hellos() const noexcept { return hellos_.load(); }

    [[nodiscard]] int closed() const noexcept { return closed_.load(); }

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
            if (stop_.load()) {
                return;
            }
            ++accepted_;
            if (mode_ == Mode::RESET) {
                // Abort only after the client's first byte lands, so the reset
                // never races the connect itself. Wait without a total budget:
                // a loaded runner may take arbitrarily long to schedule the
                // client's write, and stop_ still bounds the wait at teardown.
                for (;;) {
                    pollfd descriptor{connection.get(), POLLIN, 0};
                    if (::poll(&descriptor, 1, 50) > 0) {
                        break;
                    }
                    if (stop_.load()) {
                        return;
                    }
                }
                std::array<char, 64> chunk{};
                (void) ::recv(connection.get(), chunk.data(), chunk.size(), MSG_DONTWAIT);
                const struct linger abort_linger{.l_onoff = 1, .l_linger = 0};
                ::setsockopt(connection.get(), SOL_SOCKET, SO_LINGER, &abort_linger, sizeof(abort_linger));
                continue;  // UniqueFd closes the connection, which aborts it
            }
            if (mode_ == Mode::CAPTURE || mode_ == Mode::CAPTURE_HOLD) {
                if (capture(connection.get()) && mode_ == Mode::CAPTURE_HOLD) {
                    std::array<char, 1024> discard{};
                    while (!stop_.load()) {
                        pollfd descriptor{connection.get(), POLLIN, 0};
                        if (::poll(&descriptor, 1, 50) > 0) {
                            const auto received =
                                ::recv(connection.get(), discard.data(), discard.size(), MSG_DONTWAIT);
                            if (received == 0) {
                                ++closed_;
                                break;
                            }
                            if (received < 0) {
                                return;
                            }
                        }
                    }
                }
            }
            if (mode_ == Mode::HOLD) {
                while (!stop_.load()) {
                    std::this_thread::sleep_for(2ms);
                }
            }
        }
    }

    bool capture(const int fd) {
        std::array<std::uint8_t, 1024> chunk{};
        std::vector<std::uint8_t> wire;
        while (!stop_.load()) {
            pollfd descriptor{fd, POLLIN, 0};
            // This poll interval only admits teardown; silence is never success.
            if (::poll(&descriptor, 1, 50) <= 0) {
                continue;
            }
            const ssize_t received = ::recv(fd, chunk.data(), chunk.size(), MSG_DONTWAIT);
            if (received <= 0) {
                return false;
            }
            wire.insert(wire.end(), chunk.begin(), chunk.begin() + received);
            {
                const std::lock_guard guard(mutex_);
                captured_ = wire;
            }
            const auto frame = client_hello_frame(wire);
            if (frame == HelloFrame::INVALID) {
                return false;
            }
            if (frame == HelloFrame::COMPLETE) {
                ++hellos_;
                return true;
            }
        }
        return false;
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
        EXPECT_EQ(::connect(wake, reinterpret_cast<sockaddr*>(&storage), length), 0);
        ::close(wake);
    }

    Mode mode_;
    int family_ = AF_INET;
    std::uint16_t port_ = 0;
    Utils::UniqueFd listener_;
    std::jthread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> accepted_{0};
    std::atomic<int> hellos_{0};
    std::atomic<int> closed_{0};
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> captured_{};
};

/// Real I/O gets a system-clock failure watchdog, not a virtual-time race.
void run_task(coro::Task<void> task) {
    coro::Loop loop;
    const auto outcome =
        coro::run(loop, coro::with_timeout(2s, [&task]() -> coro::Task<void> { co_await std::move(task); }));
    EXPECT_FALSE(outcome.timed_out);
}

/// A client trust context that does no certificate work: these tests never get
/// far enough to verify anything.
[[nodiscard]] std::expected<std::shared_ptr<const net::TlsContext>, IoError> test_context() {
    return net::TlsContext::create(net::TlsTrustOptions{.verify_peer = false});
}

// ---------------------------------------------------------------------------
// TlsStream — the options decide the session, before any TLS byte is written
// ---------------------------------------------------------------------------

TEST(TlsCapture, framing_WaitsForTheWholeRecordAndClientHello) {
    // A ClientHello handshake header declaring a two-byte body.
    constexpr std::array<std::uint8_t, 11> hello{0x16, 3, 1, 0, 6, 1, 0, 0, 2, 3, 3};
    for (std::size_t length = 0; length < hello.size(); ++length) {
        EXPECT_EQ(client_hello_frame(std::span{hello}.first(length)), HelloFrame::INCOMPLETE);
    }
    EXPECT_EQ(client_hello_frame(hello), HelloFrame::COMPLETE);

    // Split the handshake header itself across two complete TLS records.
    constexpr std::array<std::uint8_t, 16> split{0x16, 3, 1, 0, 2, 1, 0, 0x16, 3, 1, 0, 4, 0, 2, 3, 3};
    EXPECT_EQ(client_hello_frame(std::span{split}.first(7)), HelloFrame::INCOMPLETE);
    EXPECT_EQ(client_hello_frame(split), HelloFrame::COMPLETE);
    auto truncated_body = hello;
    truncated_body[8] = 3;
    EXPECT_EQ(client_hello_frame(truncated_body), HelloFrame::INCOMPLETE);
    auto wrong_type = hello;
    wrong_type[5] = 2;
    EXPECT_EQ(client_hello_frame(wrong_type), HelloFrame::INVALID);
    wrong_type[0] = 0x17;
    EXPECT_EQ(client_hello_frame(wrong_type), HelloFrame::INVALID);
}

TEST(TlsCapture, splitRecords_PreserveSniAndAlpnAndOwnTheExtensionBytes) {
    constexpr std::uint16_t SNI = 0;
    constexpr std::uint16_t ALPN = 16;
    for (const bool with_sni : {false, true}) {
        std::vector<std::uint8_t> extensions{0, 16, 0, 5, 0, 3, 2, 'h', '2'};
        if (with_sni) {
            const std::array<std::uint8_t, 10> sni{0, 0, 0, 6, 0, 4, 0, 0, 1, 'a'};
            extensions.insert(extensions.end(), sni.begin(), sni.end());
        }
        // Handshake header, legacy_version and random, then empty session ID,
        // one cipher suite, null compression and the declared extensions.
        std::vector<std::uint8_t> hello(4 + 2 + 32, 0);
        hello[0] = 1;
        hello[4] = 3;
        hello[5] = 3;
        hello.insert(hello.end(), {0, 0, 2, 0x13, 1, 1, 0, 0, static_cast<std::uint8_t>(extensions.size())});
        hello.insert(hello.end(), extensions.begin(), extensions.end());
        hello[3] = static_cast<std::uint8_t>(hello.size() - 4);

        // Exercise every boundary, including inside the handshake header,
        // variable-length fields, extension headers and extension payloads.
        for (std::size_t split = 1; split < hello.size(); ++split) {
            SCOPED_TRACE(split);
            std::vector<std::uint8_t> wire;
            const auto append_record = [&wire](std::span<const std::uint8_t> payload) {
                wire.insert(wire.end(), {0x16, 3, 1, 0, static_cast<std::uint8_t>(payload.size())});
                wire.insert(wire.end(), payload.begin(), payload.end());
            };
            append_record(std::span{hello}.first(split));
            EXPECT_EQ(client_hello_frame(wire), HelloFrame::INCOMPLETE);
            EXPECT_FALSE(client_hello_extensions(wire).has_value());
            append_record(std::span{hello}.subspan(split));
            EXPECT_EQ(client_hello_frame(wire), HelloFrame::COMPLETE);
            EXPECT_EQ(classify_client_hello(wire), with_sni ? ClientHelloShape::WITH_SNI : ClientHelloShape::NO_SNI);
            EXPECT_EQ(client_hello_has_extension(wire, SNI), with_sni);
            EXPECT_TRUE(client_hello_has_extension(wire, ALPN));
            EXPECT_FALSE(client_hello_has_extension(wire, 0xffff));
            const auto parsed = client_hello_extensions(wire);
            ASSERT_TRUE(parsed.has_value());
            wire.clear();
            wire.shrink_to_fit();
            EXPECT_EQ(*parsed, extensions);  // independent of both source and reassembly lifetimes
        }
    }
}

TEST(TlsCapture, stop_InterruptsAcceptAndIncompleteCaptureWithoutClaimingCompletion) {
    {
        LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE};
        ASSERT_TRUE(peer.valid());
        peer.stop();
        EXPECT_EQ(peer.hellos(), 0);
    }
    for (const std::size_t bytes : {std::size_t{0}, std::size_t{1}, std::size_t{6}}) {
        LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE};
        ASSERT_TRUE(peer.valid());
        const Utils::UniqueFd client{::socket(AF_INET, SOCK_STREAM, 0)};
        ASSERT_TRUE(client);
        sockaddr_storage address{};
        const auto length = encode_loopback(loopback_v4(), peer.port(), address);
        ASSERT_EQ(::connect(client.get(), reinterpret_cast<sockaddr*>(&address), length), 0);
        constexpr std::array<std::uint8_t, 6> partial{0x16, 3, 1, 0, 6, 1};
        if (bytes != 0) {
            ASSERT_EQ(::send(client.get(), partial.data(), bytes, 0), static_cast<ssize_t>(bytes));
        }
        auto admitted = [&]() -> coro::Task<void> {
            while (peer.accepted() != 1 || peer.captured().size() < bytes) {
                co_await coro::checkpoint();
            }
        };
        run_task(admitted());
        // Keep the client open: only teardown, not EOF or quiet, releases capture.
        peer.stop();
        EXPECT_EQ(peer.hellos(), 0);
    }
}

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
    LoopbackPeer peer{loopback_v4(), LoopbackPeer::Mode::CAPTURE_HOLD};
    ASSERT_TRUE(peer.valid());
    auto context = test_context();
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), peer.port(), *context};

    auto task = [&stream, &peer]() -> coro::Task<void> {
        // Real TCP must be admitted by real I/O, not a ManualClock auto-jump.
        // run_task's deadline is only a failure watchdog.
        for (int attempt = 1; attempt <= 2; ++attempt) {
            bool returned = false;
            const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                auto connect = [&]() -> coro::Task<void> {
                    (void) co_await stream.ensure_connected();
                    returned = true;
                };
                co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
                    nursery.spawn_discard(connect());
                    while (peer.hellos() < attempt && !returned) {
                        co_await coro::checkpoint();
                    }
                    // SSL_connect writes the hello and registers its fd wait
                    // without yielding in between. This sibling can only
                    // observe the hello after that handshake wait is parked.
                    EXPECT_EQ(peer.hellos(), attempt);
                    EXPECT_FALSE(returned);
                    EXPECT_TRUE(stream.connected());  // TCP is live; TLS has not completed.
                    scope.cancel();
                });
            });
            EXPECT_TRUE(outcome.cancelled);
            EXPECT_FALSE(returned);
            EXPECT_FALSE(stream.connected());
            while (peer.closed() < attempt) {
                co_await coro::checkpoint();
            }
        }
    };
    run_task(task());

    EXPECT_EQ(peer.accepted(), 2);
    EXPECT_EQ(peer.hellos(), 2);
    EXPECT_EQ(peer.closed(), 2);
    EXPECT_FALSE(stream.connected());
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
        // The peer holds the abort until this byte lands, so the reset cannot
        // race the connect itself: it is sent once both sides are established.
        const std::array<std::uint8_t, 1> go{0};
        if (!co_await stream.send_all(go)) {
            co_return;
        }
        // The abort may land just after the write; wait until the probe sees
        // it. A reset is neither EOF (0) nor "nothing queued yet" (EAGAIN):
        // the pending socket error settles it. The attempt cap is only a
        // watchdog — the RST normally lands within microseconds — so a loaded
        // runner delaying the peer thread must not exhaust it.
        for (int attempt = 0; attempt < 30000 && stream.connected(); ++attempt) {
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