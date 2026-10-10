//
// Component tests for the coroutine transport layer (src/infrastructure/network/)
// plus the DNS wire exchange built on it.
//
// Real I/O on loopback: an in-process TCP echo server for the plain-TCP paths
// and the shared Python TLS echo server (test/component/servers/tls_echo_server.py)
// for the TLS paths, including certificate verification against a throwaway
// self-signed bundle. UDP and the DNS wire exchange are served by in-process
// datagram sockets, on IPv6 loopback where the exchange is exercised.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only inside coroutine bodies.
//

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "domain/network/inet_address.h"
#include "coro/coro.h"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "infrastructure/network/tls/context.h"
#include "infrastructure/network/tls/stream.h"
#include "infrastructure/network/transport/udp_socket.h"
#include "support/util/fd.hpp"

namespace {

using namespace std::chrono_literals;

using net::IoError;

constexpr std::size_t BUFFER_SIZE = 4096;
constexpr int TLS_ECHO_PORT = 21657;  // distinct from the legacy transport tests

[[nodiscard]] domain::InetAddress loopback_v4() {
    const auto address = domain::InetAddress::parse("127.0.0.1");
    EXPECT_TRUE(address.has_value());
    return address.value_or(domain::InetAddress{});
}

/// In-process loopback TCP server: one connection, on its own thread.
class EchoServer {
public:
    enum class Mode {
        ECHO,    ///< Echo every byte back.
        SILENT,  ///< Accept and then read without ever writing.
    };

    explicit EchoServer(const Mode mode) : mode_(mode) {
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

    ~EchoServer() { stop(); }

    EchoServer(const EchoServer&) = delete;
    EchoServer& operator=(const EchoServer&) = delete;
    EchoServer(EchoServer&&) = delete;
    EchoServer& operator=(EchoServer&&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void serve() {
        const int raw = ::accept(listener_.get(), nullptr, nullptr);
        if (raw < 0) {
            return;
        }
        Utils::UniqueFd connection{raw};
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

/// The shared Python TLS echo server, forked per test.
class TlsEchoServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        char directory[] = "/tmp/yaddnsc_net_coro_tls_XXXXXX";
        auto* created = ::mkdtemp(directory);
        if (created == nullptr) {
            GTEST_SKIP() << "mkdtemp failed";
        }
        directory_ = created;
        const std::string cert = directory_ + "/cert.pem";
        const std::string key = directory_ + "/key.pem";

        const std::string command = "openssl req -x509 -newkey rsa:2048 -keyout " + key + " -out " + cert +
                                    " -days 1 -nodes -subj /CN=127.0.0.1 "
                                    "-addext subjectAltName=IP:127.0.0.1 2>/dev/null";
        if (::system(command.c_str()) != 0) {
            GTEST_SKIP() << "failed to generate a throwaway certificate";
        }
        cert_path_ = cert;

        pid_ = ::fork();
        if (pid_ < 0) {
            GTEST_SKIP() << "fork() failed";
        }
        if (pid_ == 0) {
            ::setpgid(0, 0);
            ::execlp("python3", "python3", TEST_DATA_DIR "/tls_echo_server.py", std::to_string(TLS_ECHO_PORT).c_str(),
                     cert.c_str(), key.c_str(), nullptr);
            ::_exit(127);
        }

        if (!wait_until_accepting()) {
            stop();
            GTEST_SKIP() << "TLS echo server did not start";
        }
    }

    void TearDown() override {
        stop();
        if (!directory_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory_, error);
        }
    }

    [[nodiscard]] std::string cert_path() const { return cert_path_; }

    /// Build the trust context the stream under test needs from @p options. The
    /// test fails (rather than silently skipping) when the requested policy
    /// cannot produce a context.
    [[nodiscard]] std::shared_ptr<const net::TlsContext> client_context(const net::TlsOptions& options) {
        auto context = net::TlsContext::create(options);
        EXPECT_TRUE(context.has_value()) << "TlsContext::create failed";
        return context.value_or(nullptr);
    }

private:
    [[nodiscard]] bool wait_until_accepting() const {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
            if (probe < 0) {
                return false;
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(static_cast<std::uint16_t>(TLS_ECHO_PORT));
            const bool ready = ::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
            ::close(probe);
            if (ready) {
                return true;
            }
            std::this_thread::sleep_for(100ms);
        }
        return false;
    }

    void stop() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            ::waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }

    pid_t pid_ = -1;
    std::string directory_;
    std::string cert_path_;
};

/// Run a task to completion on a fresh loop with the system clock.
template<typename T>
T run_task(coro::Task<T> task) {
    coro::Loop loop;
    return coro::run(loop, std::move(task));
}

/// Frame a message the way tls_echo_server.py expects: a big-endian length
/// prefix followed by the payload, echoed back verbatim.
[[nodiscard]] std::vector<std::uint8_t> framed(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> message;
    const auto size = static_cast<std::uint32_t>(payload.size());
    message.push_back(static_cast<std::uint8_t>((size >> 24U) & 0xFFU));
    message.push_back(static_cast<std::uint8_t>((size >> 16U) & 0xFFU));
    message.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xFFU));
    message.push_back(static_cast<std::uint8_t>(size & 0xFFU));
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------

TEST(NetCoroTcpStream, ensure_connected_LargeTransfer_RoundTripsExactly) {
    EchoServer server{EchoServer::Mode::ECHO};
    net::TcpStream stream{loopback_v4(), server.port()};

    // Larger than one socket buffer, so both directions take the partial-I/O
    // and EAGAIN paths.
    std::vector<std::uint8_t> payload(64 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i % 251);
    }
    std::vector<std::uint8_t> reply(payload.size(), 0);
    bool ok = false;

    auto task = [&stream, &payload, &reply, &ok]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        if (!co_await stream.send_all(payload)) {
            co_return;
        }
        ok = (co_await stream.read_exact(reply)).has_value();
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(ok);
    EXPECT_EQ(reply, payload);
}

TEST(NetCoroTcpStream, read_some_SilentPeerWithTimeout_TimesOutWithoutKillingTheConnection) {
    EchoServer server{EchoServer::Mode::SILENT};
    net::TcpStream stream{loopback_v4(), server.port()};

    bool timed_out = false;
    std::optional<IoError> error;
    auto task = [&stream, &timed_out, &error]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        const auto outcome = co_await coro::with_timeout(50ms, [&stream, &error]() -> coro::Task<void> {
            std::array<std::uint8_t, 64> buffer{};
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
    // The timeout belongs to the scope; cancellation unwinds the await and
    // the connection stays usable afterwards.
    EXPECT_TRUE(stream.connected());
}

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------

TEST_F(TlsEchoServerTest, ensure_connected_VerifiedSelfSignedBundle_RoundTrips) {
    // The throwaway certificate carries SAN IP:127.0.0.1, so handing it in as
    // the CA bundle exercises the real verification path (IP identity, no SNI).
    net::TlsStream stream{loopback_v4(), TLS_ECHO_PORT, client_context({.ca_bundle = cert_path()})};

    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::uint8_t> message = framed(payload);
    std::vector<std::uint8_t> reply(message.size(), 0);
    bool ok = false;

    auto task = [&stream, &message, &reply, &ok]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        if (!co_await stream.send_all(message)) {
            co_return;
        }
        ok = (co_await stream.read_exact(reply)).has_value();
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(ok);
    EXPECT_EQ(reply, message);
    EXPECT_TRUE(stream.connected());
}

TEST_F(TlsEchoServerTest, ensure_connected_UnverifiableCertificate_FailsClosed) {
    // Default verification against the system trust store: a self-signed
    // certificate must be rejected, not silently accepted.
    auto context = net::TlsContext::create(net::TlsOptions{});
    if (!context.has_value()) {
        GTEST_SKIP() << "no system CA bundle available";
    }
    net::TlsStream stream{loopback_v4(), TLS_ECHO_PORT, std::move(*context)};

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

TEST_F(TlsEchoServerTest, ensure_connected_SniHostnameWithVerificationDisabled_Connects) {
    // Drives the name-based branch: SNI plus the OpenSSL 4 host-verification call
    // on the SSL verify parameter. The server certificate is for 127.0.0.1, so
    // verification stays off; this proves the branch itself does not fail.
    net::TlsStream stream{loopback_v4(), TLS_ECHO_PORT,
                          client_context({.sni_hostname = "dns.example.com", .verify_peer = false})};

    const std::vector<std::uint8_t> payload{7, 7};
    const std::vector<std::uint8_t> message = framed(payload);
    std::vector<std::uint8_t> reply(message.size(), 0);
    bool ok = false;

    auto task = [&stream, &message, &reply, &ok]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        if (!co_await stream.send_all(message)) {
            co_return;
        }
        ok = (co_await stream.read_exact(reply)).has_value();
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(ok);
    EXPECT_EQ(reply, message);
}

TEST_F(TlsEchoServerTest, ensure_connected_VerificationDisabled_RoundTrips) {
    net::TlsStream stream{loopback_v4(), TLS_ECHO_PORT, client_context({.verify_peer = false})};

    const std::vector<std::uint8_t> payload{42};
    const std::vector<std::uint8_t> message = framed(payload);
    std::vector<std::uint8_t> reply(message.size(), 0);
    bool ok = false;

    auto task = [&stream, &message, &reply, &ok]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        if (!co_await stream.send_all(message)) {
            co_return;
        }
        ok = (co_await stream.read_exact(reply)).has_value();
        co_return;
    };
    run_task(task());

    EXPECT_TRUE(ok);
    EXPECT_EQ(reply, message);
}

TEST_F(TlsEchoServerTest, read_some_SilentPeerWithTimeout_TimesOut) {
    // A TLS peer that never answers the application request: the handshake
    // succeeds (the server is a real TLS endpoint), then the read times out.
    net::TlsStream stream{loopback_v4(), TLS_ECHO_PORT, client_context({.verify_peer = false})};

    bool timed_out = false;
    std::optional<IoError> error;
    auto task = [&stream, &timed_out, &error]() -> coro::Task<void> {
        if (!co_await stream.ensure_connected()) {
            co_return;
        }
        // Send a length prefix promising more than follows: the echo server
        // waits for the rest and stays silent.
        const std::vector<std::uint8_t> partial{0, 0, 0, 8};
        if (!co_await stream.send_all(partial)) {
            co_return;
        }
        const auto outcome = co_await coro::with_timeout(50ms, [&stream, &error]() -> coro::Task<void> {
            std::array<std::uint8_t, 16> buffer{};
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
}

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------

TEST(NetCoroUdpSocket, send_to_ManyDatagrams_KeepsBoundariesAndSenders) {
    net::UdpSocket receiver;
    net::UdpSocket sender;
    ASSERT_TRUE(receiver.bind(loopback_v4(), 0).has_value());
    ASSERT_TRUE(sender.bind(loopback_v4(), 0).has_value());
    const auto receiver_port = receiver.local_port();
    const auto sender_port = sender.local_port();
    ASSERT_TRUE(receiver_port.has_value());
    ASSERT_TRUE(sender_port.has_value());

    constexpr int DATAGRAMS = 8;
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<std::optional<net::Datagram>> received;
    std::vector<std::vector<std::uint8_t>> raw;

    auto task = [&sender, &receiver, &sent, &received, &raw, port = *receiver_port]() -> coro::Task<void> {
        for (int i = 0; i < DATAGRAMS; ++i) {
            std::vector<std::uint8_t> payload(static_cast<std::size_t>(i + 1), static_cast<std::uint8_t>(i));
            if (!co_await sender.send_to(loopback_v4(), port, payload)) {
                co_return;
            }
            sent.push_back(payload);
        }
        for (int i = 0; i < DATAGRAMS; ++i) {
            std::vector<std::uint8_t> buffer(BUFFER_SIZE, 0);
            auto datagram = co_await receiver.recv_from(buffer);
            if (!datagram) {
                co_return;
            }
            buffer.resize(datagram->size);
            raw.push_back(std::move(buffer));
            received.push_back(*datagram);
        }
        co_return;
    };
    run_task(task());

    ASSERT_EQ(received.size(), static_cast<std::size_t>(DATAGRAMS));
    for (int i = 0; i < DATAGRAMS; ++i) {
        EXPECT_EQ(raw[static_cast<std::size_t>(i)], sent[static_cast<std::size_t>(i)]);
        EXPECT_EQ(received[static_cast<std::size_t>(i)]->port, *sender_port);
        EXPECT_EQ(received[static_cast<std::size_t>(i)]->from.to_string(), loopback_v4().to_string());
    }
}

TEST(NetCoroUdpSocket, recv_from_NoTrafficWithTimeout_TimesOut) {
    net::UdpSocket socket;
    ASSERT_TRUE(socket.bind(loopback_v4(), 0).has_value());

    bool timed_out = false;
    std::optional<IoError> error;
    auto task = [&socket, &timed_out, &error]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(50ms, [&socket, &error]() -> coro::Task<void> {
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

// ---------------------------------------------------------------------------
// DNS wire exchange (dns::detail) over an IPv6 server
// ---------------------------------------------------------------------------

[[nodiscard]] domain::InetAddress loopback_v6() {
    const auto address = domain::InetAddress::parse("::1");
    EXPECT_TRUE(address.has_value());
    return address.value_or(domain::InetAddress{});
}

/// Answer exactly one datagram with `reply`.
coro::Task<void> answer_one(net::UdpSocket& server, const std::vector<std::uint8_t>& reply, bool& answered) {
    std::array<std::uint8_t, BUFFER_SIZE> buffer{};
    const auto datagram = co_await server.recv_from(buffer);
    if (!datagram) {
        co_return;
    }
    if (const auto sent = co_await server.send_to(datagram->from, datagram->port, reply)) {
        answered = true;
    }
    co_return;
}

// Regression test: exchange_udp used to bind its socket to the IPv4 wildcard
// even for an IPv6 server, so every IPv6 classic/bootstrap query failed before
// the first send. The socket must open lazily on the send instead.
TEST(NetCoroDnsExchange, query_udp_Ipv6Server_RoundTrips) {
    net::UdpSocket server{domain::AddressFamily::IPV6};
    if (!server.bind(loopback_v6(), 0).has_value()) {
        GTEST_SKIP() << "IPv6 loopback is unavailable on this host";
    }
    const auto server_port = server.local_port();
    ASSERT_TRUE(server_port.has_value());

    const std::vector<std::uint8_t> query{0xDE, 0xAD, 0xBE, 0xEF};
    const std::vector<std::uint8_t> reply{0x01, 0x02, 0x03};
    bool answered = false;
    bool timed_out = false;
    std::vector<std::uint8_t> answer;

    auto task = [&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(2s, [&]() -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn(answer_one(server, reply, answered));
                const auto result = co_await dns::detail::query_udp(loopback_v6(), *server_port, query);
                if (result) {
                    answer = *result;
                }
                co_return;
            });
            co_return;
        });
        timed_out = outcome.timed_out;
        co_return;
    };
    run_task(task());

    EXPECT_FALSE(timed_out);
    EXPECT_TRUE(answered);
    EXPECT_EQ(answer, reply);
}

}  // namespace
