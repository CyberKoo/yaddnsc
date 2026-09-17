//
// Component tests for Transport (src/infrastructure/network/transport/).
//
// TcpStream: in-process loopback echo server.
// TlsStream: Python TLS echo server (fork/exec), mirroring the legacy
//            tls_stream_test fixture.
//
// Covers: roundtrips, idempotent ensure_connected(), read_exact, refused
// connections, cancellation during reads, TLS verification modes
// (off / explicit CA / default CA rejection).
// =============================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "infrastructure/network/transport/tcp_stream.h"
#include "infrastructure/network/transport/tls_stream.h"
#include "support/util/cancellation_token.hpp"

#include "support/fmt.hpp"

using namespace std::chrono_literals;
using Transport::IoError;

namespace {

// ===========================================================================
//  In-process TCP echo server (one echo thread per accepted connection).
// ===========================================================================

class EchoServer {
public:
    void start() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(listener_, 0);

        int one = 1;
        ASSERT_EQ(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral

        ASSERT_EQ(::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        ASSERT_EQ(::listen(listener_, 8), 0);

        socklen_t len = sizeof(addr);
        ASSERT_EQ(::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);

        running_ = true;
        accept_thread_ = std::jthread([this] { accept_loop(); });
    }

    void stop() {
        if (!running_) {
            return;
        }
        running_ = false;
        if (listener_ >= 0) {
            // Closing a fd does NOT reliably wake a blocked accept() on
            // Linux — wake it with a dummy self-connection instead.
            const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
            if (wake >= 0) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = htons(port_);
                ::connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                ::close(wake);
            }
            ::close(listener_);
            listener_ = -1;
        }
    }

    ~EchoServer() { stop(); }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void accept_loop() {
        while (running_) {
            const int conn = ::accept(listener_, nullptr, nullptr);
            if (conn < 0) {
                break;
            }
            std::jthread([conn] {
                std::array<char, 4096> buf{};
                for (;;) {
                    const ssize_t n = ::recv(conn, buf.data(), buf.size(), 0);
                    if (n <= 0) {
                        break;
                    }
                    ssize_t sent = 0;
                    while (sent < n) {
                        const ssize_t m = ::send(conn, buf.data() + sent, static_cast<size_t>(n - sent), MSG_NOSIGNAL);
                        if (m <= 0) {
                            ::close(conn);
                            return;
                        }
                        sent += m;
                    }
                }
                ::close(conn);
            }).detach();
        }
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    bool running_ = false;
    std::jthread accept_thread_;
};

// ===========================================================================
//  Python TLS echo server (self-signed cert generated via openssl CLI).
// ===========================================================================

constexpr int TLS_PORT = 21656;  // distinct from the legacy transport tests
pid_t g_server_pid = -1;
bool g_server_started = false;
std::string g_cert_path;
std::string g_key_path;

void generate_cert() {
    char dir_template[] = "/tmp/yaddnsc_net_tls_stream_test_XXXXXX";
    auto* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    g_cert_path = std::string(dir) + "/cert.pem";
    g_key_path = std::string(dir) + "/key.pem";

    const auto cmd = fmt::format(
        "openssl req -x509 -newkey rsa:2048 -keyout {} -out {} -days 1 -nodes "
        "-subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1 2>/dev/null",
        g_key_path, g_cert_path);

    const int ret = ::system(cmd.c_str());
    if (ret != 0) {
        GTEST_SKIP() << "Failed to generate TLS certificate (openssl returned " << ret << ")";
    }
}

void start_tls_server() {
    generate_cert();

    g_server_pid = ::fork();
    ASSERT_NE(g_server_pid, -1) << "fork() failed";

    if (g_server_pid == 0) {
        ::setpgid(0, 0);
        ::execlp("python3", "python3", TEST_DATA_DIR "/tls_echo_server.py", fmt::format("{}", TLS_PORT).c_str(),
                 g_cert_path.c_str(), g_key_path.c_str(), nullptr);
        ::_exit(127);
    }

    auto deadline = std::chrono::steady_clock::now() + 10s;
    bool ready = false;
    while (!ready && std::chrono::steady_clock::now() < deadline) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            break;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(TLS_PORT));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            ready = true;
        }
        ::close(fd);
        if (!ready) {
            std::this_thread::sleep_for(100ms);
        }
    }

    if (!ready) {
        ::kill(g_server_pid, SIGTERM);
        ::waitpid(g_server_pid, nullptr, 0);
        g_server_pid = -1;
        GTEST_SKIP() << "TLS echo server did not start within 10s";
        return;
    }
    g_server_started = true;
}

void stop_tls_server() {
    if (g_server_pid > 0) {
        ::kill(g_server_pid, SIGTERM);
        ::waitpid(g_server_pid, nullptr, 0);
        g_server_pid = -1;
    }
    g_server_started = false;
}

// ===========================================================================
//  Helpers
// ===========================================================================

[[nodiscard]] std::vector<std::uint8_t> bytes(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()),
            reinterpret_cast<const std::uint8_t*>(s.data() + s.size())};
}

[[nodiscard]] std::string str(const std::vector<std::uint8_t>& v) {
    return {reinterpret_cast<const char*>(v.data()), v.size()};
}

/// The Python TLS echo server speaks a framed protocol:
/// 4-byte big-endian length prefix + payload; it echoes prefix+payload and
/// then closes the connection.
[[nodiscard]] std::vector<std::uint8_t> framed(const std::string& s) {
    const auto payload = bytes(s);
    std::vector<std::uint8_t> out(4);
    const auto len = htonl(static_cast<std::uint32_t>(payload.size()));
    std::memcpy(out.data(), &len, sizeof(len));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ===========================================================================
//  Server that accepts and immediately closes — the client sees EOF right
//  after connect. Loops until stopped.
// ===========================================================================

class AcceptThenCloseServer {
public:
    void start() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(listener_, 0);

        int one = 1;
        ASSERT_EQ(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ASSERT_EQ(::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        ASSERT_EQ(::listen(listener_, 8), 0);

        socklen_t len = sizeof(addr);
        ASSERT_EQ(::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);

        running_ = true;
        accept_thread_ = std::jthread([this] { accept_loop(); });
    }

    void stop() {
        if (!running_) {
            return;
        }
        running_ = false;
        if (listener_ >= 0) {
            const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
            if (wake >= 0) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = htons(port_);
                ::connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                ::close(wake);
            }
            ::close(listener_);
            listener_ = -1;
        }
    }

    ~AcceptThenCloseServer() { stop(); }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void accept_loop() {
        while (running_) {
            const int conn = ::accept(listener_, nullptr, nullptr);
            if (conn < 0) {
                break;
            }
            ::close(conn);  // immediate EOF for the client
        }
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    bool running_ = false;
    std::jthread accept_thread_;
};

}  // namespace

// ===========================================================================
//  TcpStream over the in-process echo server
// ===========================================================================

class TcpStreamTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { server_.start(); }

    static void TearDownTestSuite() { server_.stop(); }

    inline static EchoServer server_;
};

TEST_F(TcpStreamTest, SendAndReadExact_EchoesBack) {
    Transport::TcpStream stream("127.0.0.1", server_.port(), {}, {});
    ASSERT_TRUE(stream.ensure_connected());

    ASSERT_TRUE(stream.send_all(bytes("ping")));

    std::vector<std::uint8_t> buf(4);
    ASSERT_TRUE(stream.read_exact(buf));
    EXPECT_EQ(str(buf), "ping");
}

TEST_F(TcpStreamTest, EnsureConnected_IsIdempotent) {
    Transport::TcpStream stream("127.0.0.1", server_.port(), {}, {});
    ASSERT_TRUE(stream.ensure_connected());
    // Second call must be a no-op success on the healthy connection.
    ASSERT_TRUE(stream.ensure_connected());

    ASSERT_TRUE(stream.send_all(bytes("again")));
    std::vector<std::uint8_t> buf(5);
    ASSERT_TRUE(stream.read_exact(buf));
    EXPECT_EQ(str(buf), "again");
}

TEST_F(TcpStreamTest, ReadSome_ReturnsAvailableBytes) {
    Transport::TcpStream stream("127.0.0.1", server_.port(), {}, {});
    ASSERT_TRUE(stream.ensure_connected());
    ASSERT_TRUE(stream.send_all(bytes("hello")));

    std::vector<std::uint8_t> buf(16);
    auto n = stream.read_some(buf);
    ASSERT_TRUE(n);
    EXPECT_EQ(*n, 5);
    EXPECT_EQ(str(std::vector<std::uint8_t>(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n))), "hello");
}

TEST_F(TcpStreamTest, ConnectionRefused_ReturnsConnectionFailed) {
    Transport::TcpStream stream("127.0.0.1", 1, {}, {});  // port 1: nothing listens
    const auto result = stream.ensure_connected();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST_F(TcpStreamTest, CancelDuringRead_UsingBoundToken) {
    Utils::CancellationSource source;
    Transport::TcpStream stream("127.0.0.1", server_.port(), {.read_timeout = 30s}, source.token());
    ASSERT_TRUE(stream.ensure_connected());

    std::jthread triggerrer([src = source] {
        std::this_thread::sleep_for(50ms);
        src.trigger();
    });

    std::vector<std::uint8_t> buf(1);
    const auto start = std::chrono::steady_clock::now();
    const auto result = stream.read_some(buf);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
    EXPECT_LT(elapsed, 5s);
}

// ===========================================================================
//  TlsStream over the Python TLS echo server
// ===========================================================================

class NetTlsStreamTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { start_tls_server(); }

    static void TearDownTestSuite() { stop_tls_server(); }

    void SetUp() override {
        if (!g_server_started) {
            GTEST_SKIP() << "TLS server not available";
        }
    }
};

TEST_F(NetTlsStreamTest, VerifyDisabled_EchoesBack) {
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {}, {.verify_peer = false}, {});
    ASSERT_TRUE(stream.ensure_connected());

    ASSERT_TRUE(stream.send_all(framed("tls-ping")));
    std::vector<std::uint8_t> buf(4 + 8);
    ASSERT_TRUE(stream.read_exact(buf));
    EXPECT_EQ(str(std::vector<std::uint8_t>(buf.begin() + 4, buf.end())), "tls-ping");
}

TEST_F(NetTlsStreamTest, VerifyWithExplicitCa_EchoesBack) {
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {}, {.ca_bundle = g_cert_path}, {});  // self-signed = own CA
    ASSERT_TRUE(stream.ensure_connected());

    ASSERT_TRUE(stream.send_all(framed("secure")));
    std::vector<std::uint8_t> buf(4 + 6);
    ASSERT_TRUE(stream.read_exact(buf));
    EXPECT_EQ(str(std::vector<std::uint8_t>(buf.begin() + 4, buf.end())), "secure");
}

TEST_F(NetTlsStreamTest, VerifyWithDefaultCa_SelfSignedRejected) {
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {}, {}, {});  // default CA store
    const auto result = stream.ensure_connected();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST_F(NetTlsStreamTest, CancelDuringTlsRead_ReturnsCancelled) {
    Utils::CancellationSource source;
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {.read_timeout = 30s}, {.verify_peer = false},
                                 source.token());
    ASSERT_TRUE(stream.ensure_connected());

    std::jthread triggerrer([src = source] {
        std::this_thread::sleep_for(50ms);
        src.trigger();
    });

    std::vector<std::uint8_t> buf(1);
    const auto result = stream.read_some(buf);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
}

TEST_F(NetTlsStreamTest, EnsureConnected_IsIdempotent) {
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {}, {.verify_peer = false}, {});
    ASSERT_TRUE(stream.ensure_connected());
    ASSERT_TRUE(stream.ensure_connected());

    ASSERT_TRUE(stream.send_all(framed("twice")));
    std::vector<std::uint8_t> buf(4 + 5);
    ASSERT_TRUE(stream.read_exact(buf));
    EXPECT_EQ(str(std::vector<std::uint8_t>(buf.begin() + 4, buf.end())), "twice");
}

// ===========================================================================
//  Additional TcpStream paths: EOF, reconnect, empty reads
// ===========================================================================

class NetTransportCloseTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { server_.start(); }

    static void TearDownTestSuite() { server_.stop(); }

    inline static AcceptThenCloseServer server_;
};

TEST_F(NetTransportCloseTest, TcpStream_PeerClose_ReadReturnsConnectionFailed) {
    Transport::TcpStream stream("127.0.0.1", server_.port(), {}, {});
    ASSERT_TRUE(stream.ensure_connected());  // accepted, then closed by the server

    std::vector<std::uint8_t> buf(4);
    const auto result = stream.read_some(buf);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);  // EOF
}

TEST_F(NetTransportCloseTest, TcpStream_UnhealthyPeer_EnsureConnectedReconnects) {
    Transport::TcpStream stream("127.0.0.1", server_.port(), {}, {});
    ASSERT_TRUE(stream.ensure_connected());

    std::vector<std::uint8_t> buf(4);
    ASSERT_FALSE(stream.read_some(buf));  // peer closed -> connection unhealthy

    // is_connected() is still true, but the EOF health probe must force a
    // reconnect — which the accept/close server satisfies again.
    EXPECT_TRUE(stream.ensure_connected());
}

TEST_F(TcpStreamTest, ReadSome_EmptyBuffer_ReturnsZero) {
    Transport::TcpStream stream("127.0.0.1", server_.port(), {}, {});
    ASSERT_TRUE(stream.ensure_connected());

    std::span<std::uint8_t> empty;
    const auto n = stream.read_some(empty);
    ASSERT_TRUE(n);
    EXPECT_EQ(*n, 0);
}

// ===========================================================================
//  Additional TlsStream paths: close_notify, buffered-data health, SNI/ALPN
// ===========================================================================

TEST_F(NetTlsStreamTest, ReadAfterServerClose_ReturnsConnectionFailed) {
    // The Python echo server closes the connection right after echoing.
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {}, {.verify_peer = false}, {});
    ASSERT_TRUE(stream.ensure_connected());

    ASSERT_TRUE(stream.send_all(framed("bye")));
    std::vector<std::uint8_t> buf(4 + 3);
    ASSERT_TRUE(stream.read_exact(buf));

    // The peer sent close_notify: the next read must surface CONNECTION_FAILED
    // (SSL_ERROR_ZERO_RETURN), not success.
    std::vector<std::uint8_t> more(1);
    const auto result = stream.read_some(more);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CONNECTION_FAILED);
}

TEST_F(NetTlsStreamTest, EnsureConnected_WithBufferedData_StaysOnConnection) {
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {}, {.verify_peer = false}, {});
    ASSERT_TRUE(stream.ensure_connected());

    // The server sends the whole frame (4-byte prefix + payload) in one TLS
    // record and then closes. Reading only the prefix leaves decrypted bytes
    // in OpenSSL's buffer — the health probe must treat the connection as
    // alive (SSL_pending > 0) even though the socket already shows EOF.
    ASSERT_TRUE(stream.send_all(framed("buffered")));
    std::vector<std::uint8_t> prefix(4);
    ASSERT_TRUE(stream.read_exact(prefix));

    EXPECT_TRUE(stream.ensure_connected());
}

TEST_F(NetTlsStreamTest, SniHostname_SetsSniAndVerificationHost) {
    // A DNS name (not an IP literal) drives the SNI + hostname-verification
    // branches. Verification is disabled here — the server cert is for
    // 127.0.0.1 — but SSL_set_tlsext_host_name / SSL_set1_host still run.
    // The SNI name is never resolved: TCP still targets 127.0.0.1.
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {}, {.sni_hostname = "dns.example.com", .verify_peer = false},
                                 {});
    ASSERT_TRUE(stream.ensure_connected());

    ASSERT_TRUE(stream.send_all(framed("named")));
    std::vector<std::uint8_t> buf(4 + 5);
    ASSERT_TRUE(stream.read_exact(buf));
    EXPECT_EQ(str(std::vector<std::uint8_t>(buf.begin() + 4, buf.end())), "named");
}

TEST_F(NetTlsStreamTest, ScopedIpv6Sni_StripScopeBeforeIpVerification) {
    // A scoped IPv6 literal must have the "%zone" stripped before
    // X509_VERIFY_PARAM_set1_ip_asc (it parses addresses, not scopes).
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {}, {.sni_hostname = "fe80::1%eth0", .verify_peer = false},
                                 {});
    ASSERT_TRUE(stream.ensure_connected());
}

TEST_F(NetTlsStreamTest, AlpnProto_SentDuringHandshake) {
    // Wire-format ALPN protocol list: one protocol "h2".
    static constexpr unsigned char alpn[] = {2, 'h', '2'};
    Transport::TlsStream stream("127.0.0.1", TLS_PORT, {}, {.alpn_proto = alpn, .verify_peer = false}, {});
    ASSERT_TRUE(stream.ensure_connected());

    ASSERT_TRUE(stream.send_all(framed("alpn")));
    std::vector<std::uint8_t> buf(4 + 4);
    ASSERT_TRUE(stream.read_exact(buf));
    EXPECT_EQ(str(std::vector<std::uint8_t>(buf.begin() + 4, buf.end())), "alpn");
}
