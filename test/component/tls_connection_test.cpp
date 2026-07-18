//
// Component tests for src/network/tls_connection.cpp
//
// Starts a Python TLS echo server on loopback, generates a self-signed
// certificate, and exercises TlsConnection::connect/send_all/read_exact
// /shutdown/is_healthy.
//
// The TLS server is started once per test suite (SetUpTestSuite) and
// stopped after all tests (TearDownTestSuite).
// =============================================================================

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include "network/tls_connection.h"
#include "exception/tls.h"
#include "util/cancellation_token.hpp"
#include "fmt.hpp"

using namespace std::chrono_literals;

namespace {

constexpr int TLS_PORT = 21653;
static pid_t server_pid = -1;
static bool server_started = false;
static std::string cert_path;
static std::string key_path;

/// Generate a self-signed certificate and key for testing.
void generate_cert() {
    auto tmp_dir = "/tmp/yaddnsc_tls_test_XXXXXX";
    char dir_template[] = "/tmp/yaddnsc_tls_test_XXXXXX";
    auto *dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    cert_path = std::string(dir) + "/cert.pem";
    key_path = std::string(dir) + "/key.pem";

    auto cmd = fmt::format(
        "openssl req -x509 -newkey rsa:2048 -keyout {} -out {} -days 1 -nodes "
        "-subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1 "
        "-addext basicConstraints=critical,CA:TRUE 2>/dev/null",
        key_path, cert_path);

    int ret = ::system(cmd.c_str());
    if (ret != 0) {
        GTEST_SKIP() << "Failed to generate TLS certificate (openssl returned " << ret << ")";
    }
}

/// Factory that creates an SSL_CTX with verification disabled for testing.
SslCtxPtr make_test_ssl_ctx() {
    SslCtxPtr ctx(SSL_CTX_new(TLS_client_method()));
    if (!ctx) return nullptr;
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
    return ctx;
}

void start_tls_server() {
    generate_cert();

    server_pid = ::fork();
    ASSERT_NE(server_pid, -1) << "fork() failed";

    if (server_pid == 0) {
        ::setpgid(0, 0);
#ifdef __linux__
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        int log_fd = ::open("/tmp/yaddnsc-tls-server.log",
                            O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
            ::close(log_fd);
        }
        // Try venv python first, then system.
        ::execlp("python3", "python3", TEST_DATA_DIR "/tls_echo_server.py",
                 fmt::format("{}", TLS_PORT).c_str(),
                 cert_path.c_str(), key_path.c_str(), nullptr);
        ::execl("/tmp/sim-venv/bin/python3", "python3", TEST_DATA_DIR "/tls_echo_server.py",
                fmt::format("{}", TLS_PORT).c_str(),
                cert_path.c_str(), key_path.c_str(), nullptr);
        ::_exit(127);
    }

    // Wait for server to become reachable via TCP.
    auto deadline = std::chrono::steady_clock::now() + 10s;
    bool ready = false;

    while (!ready && std::chrono::steady_clock::now() < deadline) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(TLS_PORT));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            ready = true;
        }
        ::close(fd);
        if (!ready) std::this_thread::sleep_for(100ms);
    }

    if (!ready) {
        ::kill(server_pid, SIGTERM);
        ::waitpid(server_pid, nullptr, 0);
        server_pid = -1;
        GTEST_SKIP() << "TLS echo server did not start within 10s";
        return;
    }

    server_started = true;
}

void stop_tls_server() {
    if (server_pid > 0) {
        ::kill(server_pid, SIGTERM);
        ::waitpid(server_pid, nullptr, 0);
        server_pid = -1;
    }
    server_started = false;
}

// ===========================================================================
// Test fixture
// ===========================================================================

class TlsConnectionTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        start_tls_server();
    }

    static void TearDownTestSuite() {
        stop_tls_server();
    }

    void SetUp() override {
        if (!server_started) {
            GTEST_SKIP() << "TLS server not available";
        }
    }
};

// ===========================================================================
// Test cases
// ===========================================================================

TEST_F(TlsConnectionTest, ConnectAndSendReceive) {
    TlsOptions opts;
    opts.connect_timeout = 5s;
    opts.read_timeout = 5s;
    opts.write_timeout = 5s;

    TlsConnection conn("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);

    // Connect
    auto conn_result = conn.connect();
    ASSERT_TRUE(conn_result.has_value()) << "TLS connect failed";

    // Verify connection is healthy
    EXPECT_TRUE(conn.is_healthy());

    // Send data: 4-byte length prefix + payload
    std::string payload = "Hello TLS!";
    std::vector<std::uint8_t> send_buf;
    uint32_t be_len = htonl(static_cast<uint32_t>(payload.size()));
    send_buf.insert(send_buf.end(),
                    reinterpret_cast<std::uint8_t *>(&be_len),
                    reinterpret_cast<std::uint8_t *>(&be_len) + 4);
    send_buf.insert(send_buf.end(), payload.begin(), payload.end());

    Utils::CancellationToken cancel;
    auto send_result = conn.send_all(send_buf, cancel);
    ASSERT_TRUE(send_result.has_value()) << "send_all failed";

    // Read back the echo
    std::vector<std::uint8_t> recv_buf(send_buf.size());
    auto read_result = conn.read_exact(recv_buf, cancel);
    ASSERT_TRUE(read_result.has_value()) << "read_exact failed";

    // Verify echo matches
    EXPECT_EQ(recv_buf, send_buf);
}

TEST_F(TlsConnectionTest, IsHealthy_AfterConnect) {
    TlsOptions opts;
    opts.connect_timeout = 5s;

    TlsConnection conn("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    auto r = conn.connect();
    ASSERT_TRUE(r.has_value());

    EXPECT_TRUE(conn.is_healthy());
}

TEST_F(TlsConnectionTest, IsHealthy_BeforeConnect_ReturnsFalse) {
    TlsOptions opts;
    TlsConnection conn("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    EXPECT_FALSE(conn.is_healthy());
}

TEST_F(TlsConnectionTest, IsHealthy_AfterClose_ReturnsFalse) {
    TlsOptions opts;
    opts.connect_timeout = 5s;

    TlsConnection conn("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    auto r = conn.connect();
    ASSERT_TRUE(r.has_value());

    conn.close();
    EXPECT_FALSE(conn.is_healthy());
}

TEST_F(TlsConnectionTest, SendToClosedConnection_ReturnsError) {
    TlsOptions opts;
    TlsConnection conn("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    // Not connected — send should fail.
    Utils::CancellationToken cancel;
    std::vector<std::uint8_t> data{1, 2, 3};
    auto r = conn.send_all(data, cancel);
    ASSERT_FALSE(r.has_value());
}

TEST_F(TlsConnectionTest, ReadFromClosedConnection_ReturnsError) {
    TlsOptions opts;
    TlsConnection conn("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    Utils::CancellationToken cancel;
    std::vector<std::uint8_t> buf(16);
    auto r = conn.read_exact(buf, cancel);
    ASSERT_FALSE(r.has_value());
}

TEST_F(TlsConnectionTest, ConnectToRefusedPort_ReturnsError) {
    TlsOptions opts;
    opts.connect_timeout = 3s;

    // Port 1 should be closed.
    TlsConnection conn("127.0.0.1", 1, opts, make_test_ssl_ctx);
    auto r = conn.connect();
    ASSERT_FALSE(r.has_value());
}

TEST_F(TlsConnectionTest, Shutdown_AfterConnect) {
    TlsOptions opts;
    opts.connect_timeout = 5s;

    TlsConnection conn("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    auto r = conn.connect();
    ASSERT_TRUE(r.has_value());

    EXPECT_NO_THROW({ auto s = conn.shutdown(); });
}

TEST_F(TlsConnectionTest, ReadSome_Works) {
    TlsOptions opts;
    opts.connect_timeout = 5s;
    opts.read_timeout = 5s;
    opts.write_timeout = 5s;

    TlsConnection conn("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    auto cr = conn.connect();
    ASSERT_TRUE(cr.has_value());

    // Send echo request
    std::string payload = "read_some_test";
    std::vector<std::uint8_t> send_buf;
    uint32_t be_len = htonl(static_cast<uint32_t>(payload.size()));
    send_buf.insert(send_buf.end(),
                    reinterpret_cast<std::uint8_t *>(&be_len),
                    reinterpret_cast<std::uint8_t *>(&be_len) + 4);
    send_buf.insert(send_buf.end(), payload.begin(), payload.end());

    Utils::CancellationToken cancel;
    ASSERT_TRUE(conn.send_all(send_buf, cancel).has_value());

    // read_some at least part of the echo
    std::vector<std::uint8_t> part(1024);
    auto rr = conn.read_some(part, cancel);
    ASSERT_TRUE(rr.has_value());
    EXPECT_GT(*rr, 0u);
}

TEST_F(TlsConnectionTest, InvalidServerAddress_Throws) {
    TlsOptions opts;
    EXPECT_THROW(
        { TlsConnection conn("not_a_valid_address!!!", TLS_PORT, opts, make_test_ssl_ctx); },
        TlsException
    );
}

TEST_F(TlsConnectionTest, CustomSniHostname) {
    TlsOptions opts;
    opts.sni_hostname = "test.example.com";
    opts.connect_timeout = 5s;

    TlsConnection conn("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    auto r = conn.connect();
    EXPECT_TRUE(conn.is_healthy());
}

TEST_F(TlsConnectionTest, ConnectWithSystemCaBundle) {
    // Use the default SSL context (create_default_ssl_ctx) with
    // CA verification, pointing SSL_CERT_FILE at the server's cert.
    const auto *old_env = std::getenv("SSL_CERT_FILE");
    ::setenv("SSL_CERT_FILE", cert_path.c_str(), 1);

    TlsOptions opts;
    opts.connect_timeout = 5s;
    opts.read_timeout = 5s;
    opts.write_timeout = 5s;

    // No custom factory — uses real create_default_ssl_ctx().
    TlsConnection conn("127.0.0.1", TLS_PORT, opts);
    auto cr = conn.connect();
    ASSERT_TRUE(cr.has_value()) << "TLS connect with default SSL_CTX failed";
    EXPECT_TRUE(conn.is_healthy());

    // Send + receive.
    std::string payload = "Hello CA!";
    std::vector<std::uint8_t> send_buf;
    uint32_t be_len = htonl(static_cast<uint32_t>(payload.size()));
    send_buf.insert(send_buf.end(),
                    reinterpret_cast<std::uint8_t *>(&be_len),
                    reinterpret_cast<std::uint8_t *>(&be_len) + 4);
    send_buf.insert(send_buf.end(), payload.begin(), payload.end());

    Utils::CancellationToken cancel;
    auto sr = conn.send_all(send_buf, cancel);
    ASSERT_TRUE(sr.has_value()) << "send_all failed";

    std::vector<std::uint8_t> recv_buf(send_buf.size());
    auto rr = conn.read_exact(recv_buf, cancel);
    ASSERT_TRUE(rr.has_value()) << "read_exact failed";
    EXPECT_EQ(recv_buf, send_buf);

    // Restore env var.
    if (old_env) {
        ::setenv("SSL_CERT_FILE", old_env, 1);
    } else {
        ::unsetenv("SSL_CERT_FILE");
    }
}

} // anonymous namespace
