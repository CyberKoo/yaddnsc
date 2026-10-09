//
// Unit tests for the coroutine transport layer (src/infrastructure/network/) and for
// the runtime's fd-wait checkpoint they are built on.
//
// Scope: loopback/socketpair-level I/O only. A TLS handshake against a real TLS
// peer (and certificate verification) is a component test.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "domain/network/inet_address.h"
#include "infrastructure/coro/coro.h"
#include "infrastructure/coro/fd_wait.hpp"
#include "infrastructure/network/tls/context.h"
#include "infrastructure/network/tls/stream.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "infrastructure/network/transport/udp_socket.h"
#include "support/util/fd.hpp"

namespace {

using namespace std::chrono_literals;

using net::IoError;

constexpr std::size_t BUFFER_SIZE = 512;

[[nodiscard]] domain::InetAddress loopback_v4() {
    const auto address = domain::InetAddress::parse("127.0.0.1");
    EXPECT_TRUE(address.has_value());
    return address.value_or(domain::InetAddress{});
}

/// An AF_UNIX socketpair, owned by UniqueFd so a failing assertion leaks nothing.
class SocketPair {
public:
    static std::optional<SocketPair> create() {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            return std::nullopt;
        }
        return SocketPair{Utils::UniqueFd{fds[0]}, Utils::UniqueFd{fds[1]}};
    }

    SocketPair(Utils::UniqueFd first, Utils::UniqueFd second) : first_(std::move(first)), second_(std::move(second)) {}

    [[nodiscard]] int first() const noexcept { return first_.get(); }

    [[nodiscard]] int second() const noexcept { return second_.get(); }

private:
    Utils::UniqueFd first_;
    Utils::UniqueFd second_;
};

/// A one-connection loopback TCP server on its own thread.
class LoopbackServer {
public:
    enum class Mode {
        ECHO,    ///< Echo every byte back.
        SILENT,  ///< Accept and then read without ever writing.
        CLOSE,   ///< Accept and close immediately.
    };

    explicit LoopbackServer(const Mode mode) : mode_(mode) {
        listener_.reset(::socket(AF_INET, SOCK_STREAM, 0));
        const int enabled = 1;
        ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));

        socklen_t length = sizeof(address);
        ::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        ::listen(listener_.get(), 4);
        thread_ = std::jthread([this] { serve(); });
    }

    ~LoopbackServer() { stop(); }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;
    LoopbackServer(LoopbackServer&&) = delete;
    LoopbackServer& operator=(LoopbackServer&&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void serve() {
        const int raw = ::accept(listener_.get(), nullptr, nullptr);
        if (raw < 0) {
            return;
        }
        Utils::UniqueFd connection{raw};
        if (mode_ == Mode::CLOSE) {
            return;
        }

        std::array<char, BUFFER_SIZE> buffer{};
        for (;;) {
            const ssize_t received = ::recv(connection.get(), buffer.data(), buffer.size(), 0);
            if (received <= 0) {
                return;
            }
            if (mode_ == Mode::SILENT) {
                continue;
            }
            ssize_t sent = 0;
            while (sent < received) {
                const auto remaining = static_cast<std::size_t>(received - sent);
                const ssize_t wrote = ::send(connection.get(), buffer.data() + sent, remaining, 0);
                if (wrote <= 0) {
                    return;
                }
                sent += wrote;
            }
        }
    }

    void stop() {
        if (listener_) {
            // Closing an fd does not reliably wake a blocked accept() on Linux;
            // a throwaway self-connection does.
            const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
            if (wake >= 0) {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(port_);
                ::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address));
                ::close(wake);
            }
            listener_.reset();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    Mode mode_;
    std::uint16_t port_ = 0;
    Utils::UniqueFd listener_;
    std::jthread thread_;
};

/// Run a task to completion on a fresh loop with the system clock.
template<typename T>
T run_task(coro::Task<T> task) {
    coro::Loop loop;
    return coro::run(loop, std::move(task));
}

// ---------------------------------------------------------------------------
// runtime fd-wait checkpoint
// ---------------------------------------------------------------------------

TEST(FdWait, wait_readable_ByteQueued_ReportsReady) {
    auto pair = SocketPair::create();
    ASSERT_TRUE(pair.has_value());
    const char byte = 'x';
    ASSERT_EQ(::send(pair->second(), &byte, 1, 0), 1);

    bool ready = false;
    auto task = [&ready, fd = pair->first()]() -> coro::Task<void> {
        co_await coro::wait_readable(fd);
        ready = true;
        co_return;
    };
    run_task(task());
    EXPECT_TRUE(ready);
}

TEST(FdWait, wait_readable_NoData_IsCancelledByScopeTimeout) {
    auto pair = SocketPair::create();
    ASSERT_TRUE(pair.has_value());

    bool cancelled = false;
    bool timed_out = false;
    auto task = [&cancelled, &timed_out, fd = pair->first()]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(20ms, [&cancelled, fd]() -> coro::Task<void> {
            try {
                co_await coro::wait_readable(fd);
            } catch (const coro::Cancelled&) {
                cancelled = true;
                throw;
            }
            co_return;
        });
        timed_out = outcome.timed_out;
        co_return;
    };
    run_task(task());
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(timed_out);
}

TEST(FdWait, wait_writable_OpenSocket_ReportsReady) {
    auto pair = SocketPair::create();
    ASSERT_TRUE(pair.has_value());

    bool ready = false;
    auto task = [&ready, fd = pair->first()]() -> coro::Task<void> {
        co_await coro::wait_writable(fd);
        ready = true;
        co_return;
    };
    run_task(task());
    EXPECT_TRUE(ready);
}

TEST(FdWait, wait_readable_InvalidDescriptor_ThrowsContractError) {
    bool cancelled = false;
    auto task = [&cancelled]() -> coro::Task<void> {
        try {
            co_await coro::wait_readable(-1);
        } catch (const std::logic_error&) {
            cancelled = true;
        }
        co_return;
    };
    run_task(task());
    EXPECT_TRUE(cancelled);
}

// ---------------------------------------------------------------------------
// TcpStream
// ---------------------------------------------------------------------------

TEST(TcpStream, ensure_connected_LoopbackServer_EchoesRoundTrip) {
    LoopbackServer server{LoopbackServer::Mode::ECHO};
    net::TcpStream stream{loopback_v4(), server.port()};

    constexpr std::array<std::uint8_t, 5> REQUEST{1, 2, 3, 4, 5};
    std::array<std::uint8_t, REQUEST.size()> reply{};
    bool ok = false;

    auto task = [&stream, &reply, &ok, REQUEST]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        if (!co_await stream.send_all(REQUEST)) {
            co_return;
        }
        const auto read = co_await stream.read_exact(reply);
        ok = read.has_value();
        co_return;
    };
    run_task(task());
    EXPECT_TRUE(ok);
    EXPECT_EQ(reply, REQUEST);
}

TEST(TcpStream, ensure_connected_Twice_StaysConnected) {
    LoopbackServer server{LoopbackServer::Mode::ECHO};
    net::TcpStream stream{loopback_v4(), server.port()};

    bool first = false;
    bool second = false;
    bool still_connected = false;

    auto task = [&stream, &first, &second, &still_connected]() -> coro::Task<void> {
        first = (co_await stream.ensure_connected()).has_value();
        second = (co_await stream.ensure_connected()).has_value();
        still_connected = stream.connected();
        co_return;
    };
    run_task(task());
    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
    EXPECT_TRUE(still_connected);
}

TEST(TcpStream, ensure_connected_RefusedPort_Fails) {
    // A listener that has already closed leaves a port nothing answers on.
    std::uint16_t closed_port = 0;
    {
        LoopbackServer server{LoopbackServer::Mode::ECHO};
        closed_port = server.port();
    }

    net::TcpStream stream{loopback_v4(), closed_port};
    std::optional<IoError> error;
    auto task = [&stream, &error]() -> coro::Task<void> {
        const auto result = co_await stream.ensure_connected();
        if (!result) {
            error = result.error();
        }
        co_return;
    };
    run_task(task());
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
    EXPECT_FALSE(stream.connected());
}

TEST(TcpStream, read_some_PeerClosed_Fails) {
    LoopbackServer server{LoopbackServer::Mode::CLOSE};
    net::TcpStream stream{loopback_v4(), server.port()};

    std::optional<IoError> error;
    auto task = [&stream, &error]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        std::array<std::uint8_t, 8> buffer{};
        const auto result = co_await stream.read_some(buffer);
        if (!result) {
            error = result.error();
        }
        co_return;
    };
    run_task(task());
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
}

TEST(TcpStream, read_some_ScopeTimesOut_ReportsCancelledAndTimedOut) {
    LoopbackServer server{LoopbackServer::Mode::SILENT};
    net::TcpStream stream{loopback_v4(), server.port()};

    std::optional<IoError> error;
    bool timed_out = false;
    auto task = [&stream, &error, &timed_out]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        const auto outcome = co_await coro::with_timeout(30ms, [&stream, &error]() -> coro::Task<void> {
            std::array<std::uint8_t, 8> buffer{};
            const auto result = co_await stream.read_some(buffer);
            if (!result) {
                error = result.error();
            }
            co_return;
        });
        timed_out = outcome.timed_out;
        co_return;
    };
    run_task(task());
    EXPECT_TRUE(timed_out);
    EXPECT_FALSE(error.has_value());  // cancellation bypasses the recoverable error channel
    // A cancelled wait leaves the connection usable.
    EXPECT_TRUE(stream.connected());
}

TEST(TcpStream, read_exact_ShortMessage_Fails) {
    LoopbackServer server{LoopbackServer::Mode::CLOSE};
    net::TcpStream stream{loopback_v4(), server.port()};

    std::optional<IoError> error;
    auto task = [&stream, &error]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        std::array<std::uint8_t, 8> buffer{};
        const auto result = co_await stream.read_exact(buffer);
        if (!result) {
            error = result.error();
        }
        co_return;
    };
    run_task(task());
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
}

TEST(TcpStream, read_some_BeforeConnecting_Fails) {
    net::TcpStream stream{loopback_v4(), 9};
    std::optional<IoError> error;
    auto task = [&stream, &error]() -> coro::Task<void> {
        std::array<std::uint8_t, 4> buffer{};
        const auto result = co_await stream.read_some(buffer);
        if (!result) {
            error = result.error();
        }
        co_return;
    };
    run_task(task());
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
}

// ---------------------------------------------------------------------------
// TlsStream — failure paths only; a real handshake is a component test.
// ---------------------------------------------------------------------------

TEST(TlsStream, ensure_connected_PlainPeer_FailsHandshake) {
    LoopbackServer server{LoopbackServer::Mode::CLOSE};
    auto context = net::TlsContext::create(net::TlsOptions{.verify_peer = false});
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), server.port(), std::move(*context)};

    std::optional<IoError> error;
    auto task = [&stream, &error]() -> coro::Task<void> {
        const auto result = co_await stream.ensure_connected();
        if (!result) {
            error = result.error();
        }
        co_return;
    };
    run_task(task());
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
    EXPECT_FALSE(stream.connected());
}

TEST(TlsStream, read_some_BeforeConnecting_Fails) {
    auto context = net::TlsContext::create(net::TlsOptions{.verify_peer = false});
    ASSERT_TRUE(context.has_value());
    net::TlsStream stream{loopback_v4(), 9, std::move(*context)};
    std::optional<IoError> error;
    auto task = [&stream, &error]() -> coro::Task<void> {
        std::array<std::uint8_t, 4> buffer{};
        const auto result = co_await stream.read_some(buffer);
        if (!result) {
            error = result.error();
        }
        co_return;
    };
    run_task(task());
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(*error, IoError::CONNECTION_FAILED);
}

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

TEST(UdpSocket, send_to_recv_from_LoopbackRoundTrip) {
    net::UdpSocket receiver;
    net::UdpSocket sender;
    ASSERT_TRUE(receiver.bind(loopback_v4(), 0).has_value());
    ASSERT_TRUE(sender.bind(loopback_v4(), 0).has_value());
    const auto receiver_port = receiver.local_port();
    ASSERT_TRUE(receiver_port.has_value());

    constexpr std::array<std::uint8_t, 4> PAYLOAD{9, 8, 7, 6};
    std::array<std::uint8_t, BUFFER_SIZE> buffer{};
    std::optional<net::Datagram> received;
    bool sent = false;

    auto task = [&sender, &receiver, &buffer, &received, &sent, port = *receiver_port, PAYLOAD]() -> coro::Task<void> {
        sent = (co_await sender.send_to(loopback_v4(), port, PAYLOAD)).has_value();
        auto datagram = co_await receiver.recv_from(buffer);
        if (datagram) {
            received = *datagram;
        }
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(sent);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->size, PAYLOAD.size());
    EXPECT_EQ(received->from.to_string(), loopback_v4().to_string());
    EXPECT_EQ(received->port, *sender.local_port());
    EXPECT_TRUE(std::equal(PAYLOAD.begin(), PAYLOAD.end(), buffer.begin()));
}

TEST(UdpSocket, recv_from_ScopeTimesOut_ReportsCancelled) {
    net::UdpSocket socket;
    ASSERT_TRUE(socket.bind(loopback_v4(), 0).has_value());

    std::optional<IoError> error;
    bool timed_out = false;
    auto task = [&socket, &error, &timed_out]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(30ms, [&socket, &error]() -> coro::Task<void> {
            std::array<std::uint8_t, BUFFER_SIZE> buffer{};
            const auto result = co_await socket.recv_from(buffer);
            if (!result) {
                error = result.error();
            }
            co_return;
        });
        timed_out = outcome.timed_out;
        co_return;
    };
    run_task(task());
    EXPECT_TRUE(timed_out);
    EXPECT_FALSE(error.has_value());  // cancellation bypasses the recoverable error channel
}

}  // namespace
