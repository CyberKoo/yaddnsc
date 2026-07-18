//
// Component tests for src/network/transport/tls_stream.cpp
//
// Starts a Python TLS echo server on loopback, exercises TlsStream
// read/write operations and error mapping.
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

#include <gtest/gtest.h>

#include "network/transport/tls_stream.h"
#include "network/tls_connection.h"
#include "exception/tls.h"
#include "util/cancellation_token.hpp"
#include "fmt.hpp"

using namespace std::chrono_literals;

namespace {

constexpr int TLS_PORT = 21654;  // different port from tls_connection_test
static pid_t server_pid = -1;
static bool server_started = false;
static std::string cert_path;
static std::string key_path;

void generate_cert() {
    char dir_template[] = "/tmp/yaddnsc_tls_stream_test_XXXXXX";
    auto *dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    cert_path = std::string(dir) + "/cert.pem";
    key_path = std::string(dir) + "/key.pem";

    auto cmd = fmt::format(
        "openssl req -x509 -newkey rsa:2048 -keyout {} -out {} -days 1 -nodes "
        "-subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1 2>/dev/null",
        key_path, cert_path);

    int ret = ::system(cmd.c_str());
    if (ret != 0) {
        GTEST_SKIP() << "Failed to generate TLS certificate (openssl returned " << ret << ")";
    }
}

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
        int log_fd = ::open("/tmp/yaddnsc-tls-stream-server.log",
                            O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
            ::close(log_fd);
        }
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

class TlsStreamTest : public ::testing::Test {
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

} // anonymous namespace

// ===========================================================================
// Test cases
// ===========================================================================

TEST_F(TlsStreamTest, SendAllAndReadExact) {
    TlsOptions opts;
    opts.connect_timeout = 5s;
    opts.read_timeout = 5s;
    opts.write_timeout = 5s;

    auto conn = std::make_unique<TlsConnection>("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    ASSERT_TRUE(conn->connect().has_value());

    Transport::TlsStream stream(*conn);

    // Send a length-prefixed message.
    std::string payload = "Hello TlsStream!";
    std::vector<std::uint8_t> send_buf;
    uint32_t be_len = htonl(static_cast<uint32_t>(payload.size()));
    send_buf.insert(send_buf.end(),
                    reinterpret_cast<std::uint8_t *>(&be_len),
                    reinterpret_cast<std::uint8_t *>(&be_len) + 4);
    send_buf.insert(send_buf.end(), payload.begin(), payload.end());

    Utils::CancellationToken cancel;
    auto send_result = stream.send_all(send_buf, cancel);
    ASSERT_TRUE(send_result.has_value()) << "send_all failed";

    // Read back the echo (length prefix + payload).
    std::vector<std::uint8_t> recv_buf(send_buf.size());
    auto read_result = stream.read_exact(recv_buf, cancel);
    ASSERT_TRUE(read_result.has_value()) << "read_exact failed";

    EXPECT_EQ(recv_buf, send_buf);
}

TEST_F(TlsStreamTest, SendAllAndReadSome) {
    TlsOptions opts;
    opts.connect_timeout = 5s;
    opts.read_timeout = 5s;
    opts.write_timeout = 5s;

    auto conn = std::make_unique<TlsConnection>("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    ASSERT_TRUE(conn->connect().has_value());

    Transport::TlsStream stream(*conn);

    // Send a small message.
    std::string msg = "data";
    std::vector<std::uint8_t> send_buf;
    uint32_t be_len = htonl(static_cast<uint32_t>(msg.size()));
    send_buf.insert(send_buf.end(),
                    reinterpret_cast<std::uint8_t *>(&be_len),
                    reinterpret_cast<std::uint8_t *>(&be_len) + 4);
    send_buf.insert(send_buf.end(), msg.begin(), msg.end());

    Utils::CancellationToken cancel;
    ASSERT_TRUE(stream.send_all(send_buf, cancel).has_value());

    // Read back with read_some (partial read).
    std::vector<std::uint8_t> recv_buf(send_buf.size());
    auto n = stream.read_some(recv_buf, cancel);
    ASSERT_TRUE(n.has_value()) << "read_some failed";
    EXPECT_EQ(*n, send_buf.size());
    EXPECT_EQ(recv_buf, send_buf);
}

TEST_F(TlsStreamTest, ReadCancelled_ReturnsIoErrorCancelled) {
    TlsOptions opts;
    opts.connect_timeout = 5s;
    opts.read_timeout = 5s;

    auto conn = std::make_unique<TlsConnection>("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    ASSERT_TRUE(conn->connect().has_value());

    Transport::TlsStream stream(*conn);

    // Cancel before reading.
    Utils::CancellationSource src;
    src.trigger();

    std::vector<std::uint8_t> buf(16);
    auto result = stream.read_some(buf, src.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::CANCELLED);
}

TEST_F(TlsStreamTest, ReadExactCancelled_ReturnsIoErrorCancelled) {
    TlsOptions opts;
    opts.connect_timeout = 5s;
    opts.read_timeout = 5s;

    auto conn = std::make_unique<TlsConnection>("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    ASSERT_TRUE(conn->connect().has_value());

    Transport::TlsStream stream(*conn);

    // Cancel before reading.
    Utils::CancellationSource src;
    src.trigger();

    std::vector<std::uint8_t> buf(16);
    auto result = stream.read_exact(buf, src.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::CANCELLED);
}

TEST_F(TlsStreamTest, SendAllCancelled_ReturnsIoErrorCancelled) {
    TlsOptions opts;
    opts.connect_timeout = 5s;
    opts.read_timeout = 5s;
    opts.write_timeout = 5s;

    auto conn = std::make_unique<TlsConnection>("127.0.0.1", TLS_PORT, opts, make_test_ssl_ctx);
    ASSERT_TRUE(conn->connect().has_value());

    Transport::TlsStream stream(*conn);

    // Cancel before sending.
    Utils::CancellationSource src;
    src.trigger();

    std::vector<std::uint8_t> data(4, 0);
    auto result = stream.send_all(data, src.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Transport::IoError::CANCELLED);
}
