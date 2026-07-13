//
// Component tests for src/dns/resolver/dot.cpp — DNS-over-TLS (RFC 7858)
//
// Starts a Python DoT server on loopback, generates a self-signed
// certificate, and exercises DotResolver::query().
//
// The DoT server is started once per test suite (SetUpTestSuite) and
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

#include "dns/resolver/dot.h"
#include "dns_error.h"
#include "record_kind.h"
#include "util/cancellation_token.hpp"
#include "fmt.hpp"

using namespace std::chrono_literals;

namespace {

constexpr int DOT_PORT = 21853;
static pid_t server_pid = -1;
static bool server_started = false;
static std::string cert_path;
static std::string key_path;
static std::string server_log;

/// Format a DnsError code for diagnostic messages.
[[nodiscard]] std::string_view dns_error_name(DnsError code) {
    switch (code) {
    case DnsError::NX_DOMAIN:    return "NX_DOMAIN";
    case DnsError::RETRY:        return "RETRY";
    case DnsError::NODATA:       return "NODATA";
    case DnsError::PARSE:        return "PARSE";
    case DnsError::CONNECTION:   return "CONNECTION";
    case DnsError::CONFIG:       return "CONFIG";
    case DnsError::CANCELLED:    return "CANCELLED";
    case DnsError::SERVER_REFUSED: return "SERVER_REFUSED";
    case DnsError::UNKNOWN:      return "UNKNOWN";
    }
    return "?";
}

/// Generate a self-signed certificate and key for testing.
void generate_cert() {
    char dir_template[] = "/tmp/yaddnsc_dot_test_XXXXXX";
    auto *dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    cert_path = std::string(dir) + "/cert.pem";
    key_path = std::string(dir) + "/key.pem";

    auto cmd = fmt::format(
        "openssl req -x509 -newkey rsa:2048 -keyout {} -out {} -days 1 -nodes "
        "-subj /CN=127.0.0.1 "
        "-addext subjectAltName=IP:127.0.0.1 "
        "-addext basicConstraints=critical,CA:TRUE 2>/dev/null",
        key_path, cert_path);

    int ret = ::system(cmd.c_str());
    if (ret != 0) {
        GTEST_SKIP() << "Failed to generate TLS certificate (openssl returned " << ret << ")";
    }

    // Also export the generated cert in PEM format that OpenSSL can load
    // as a trusted root via SSL_CERT_FILE.
    ::setenv("SSL_CERT_FILE", cert_path.c_str(), 1);
}

void start_dot_server() {
    generate_cert();

    server_log = "/tmp/yaddnsc-dot-server.log";

    server_pid = ::fork();
    ASSERT_NE(server_pid, -1) << "fork() failed";

    if (server_pid == 0) {
        ::setpgid(0, 0);
#ifdef __linux__
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        int log_fd = ::open(server_log.c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
            ::close(log_fd);
        }
        // Try venv python first, then system.
        ::execlp("python3", "python3", TEST_DATA_DIR "/dot_server.py",
                 fmt::format("{}", DOT_PORT).c_str(),
                 cert_path.c_str(), key_path.c_str(), nullptr);
        ::execl("/tmp/sim-venv/bin/python3", "python3", TEST_DATA_DIR "/dot_server.py",
                fmt::format("{}", DOT_PORT).c_str(),
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
        addr.sin_port = htons(static_cast<std::uint16_t>(DOT_PORT));
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
        GTEST_SKIP() << "DoT server did not start within 10s.\n"
                     << "Log: " << server_log;
        return;
    }

    server_started = true;
}

void stop_dot_server() {
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

class DotResolverTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        start_dot_server();
    }

    static void TearDownTestSuite() {
        stop_dot_server();
    }

    void SetUp() override {
        if (!server_started) {
            GTEST_SKIP() << "DoT server not available";
        }
    }
};

// ===========================================================================
// Test cases
// ===========================================================================

TEST_F(DotResolverTest, Resolve_A_Record) {
    DotResolver resolver("127.0.0.1", DOT_PORT, "test-dot");
    Utils::CancellationToken cancel;
    auto result = resolver.query("yaddnsc.test", RecordKind::A, cancel);

    ASSERT_TRUE(result.has_value()) << "DoT query failed: "
                                    << dns_error_name(result.error().code);
    ASSERT_GE(result->size(), 12U);

    // QNAME for "yaddnsc.test" = \x07yaddnsc\x04test\x00 = 14 bytes
    // RDATA starts at: 12(header) + 14(QNAME) + 4(QTYPE+QCLASS)
    //   + 2(name ptr) + 2(TYPE) + 2(CLASS) + 4(TTL) + 2(RDLENGTH) = 42
    ASSERT_GE(result->size(), 46U);
    EXPECT_EQ((*result)[42], 198);
    EXPECT_EQ((*result)[43], 51);
    EXPECT_EQ((*result)[44], 100);
    EXPECT_EQ((*result)[45], 42);
}

TEST_F(DotResolverTest, Resolve_AAAA_Record) {
    DotResolver resolver("127.0.0.1", DOT_PORT, "test-dot");
    Utils::CancellationToken cancel;
    auto result = resolver.query("yaddnsc.test", RecordKind::AAAA, cancel);

    ASSERT_TRUE(result.has_value()) << "DoT AAAA query failed: "
                                    << dns_error_name(result.error().code);
    ASSERT_GT(result->size(), 12U);
}

TEST_F(DotResolverTest, ConnectToRefusedPort_ReturnsError) {
    DotResolver resolver("127.0.0.1", 1, "test-dot-refused");
    Utils::CancellationToken cancel;
    auto result = resolver.query("yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code == DnsError::CONNECTION ||
                result.error().code == DnsError::RETRY)
        << "Expected CONNECTION or RETRY, got "
        << dns_error_name(result.error().code);
}

TEST_F(DotResolverTest, TimeoutHost_ReturnsRetry) {
    DotResolver resolver("127.0.0.1", DOT_PORT, "test-dot-timeout");
    Utils::CancellationToken cancel;
    auto result = resolver.query("dot-timeout.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code == DnsError::RETRY ||
                result.error().code == DnsError::CONNECTION ||
                result.error().code == DnsError::CANCELLED)
        << "Expected RETRY, CONNECTION, or CANCELLED, got "
        << dns_error_name(result.error().code);
}

TEST_F(DotResolverTest, MalformedResponse_ReturnsParseError) {
    DotResolver resolver("127.0.0.1", DOT_PORT, "test-dot-malformed");
    Utils::CancellationToken cancel;
    auto result = resolver.query("dot-malformed.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::PARSE);
}

TEST_F(DotResolverTest, ZeroLengthResponse_ReturnsParseError) {
    DotResolver resolver("127.0.0.1", DOT_PORT, "test-dot-zero");
    Utils::CancellationToken cancel;
    auto result = resolver.query("dot-zerolength.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::PARSE);
}

TEST_F(DotResolverTest, NonExistentDomain_FallsBackToDefault) {
    DotResolver resolver("127.0.0.1", DOT_PORT, "test-dot-nx");
    Utils::CancellationToken cancel;
    auto result = resolver.query("nonexistent.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_TRUE(result.has_value()) << "DoT query failed: "
                                    << dns_error_name(result.error().code);
    // The test server falls back to 198.51.100.1 for unknown A records.
    // QNAME for "nonexistent.yaddnsc.test" = 26 bytes
    // RDATA starts at: 12 + 26 + 4 + 2 + 2 + 2 + 4 + 2 = 54
    ASSERT_GE(result->size(), 58U);
    EXPECT_EQ((*result)[54], 198);
    EXPECT_EQ((*result)[55], 51);
    EXPECT_EQ((*result)[56], 100);
    EXPECT_EQ((*result)[57], 1);
}

} // anonymous namespace
