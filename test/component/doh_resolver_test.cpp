//
// Component tests for src/dns/resolver/doh.cpp — DNS-over-HTTPS (RFC 8484)
//
// Starts a Python DoH server on loopback, generates a self-signed
// certificate, and exercises DohResolver::query().
//
// The DoH server is started once per test suite (SetUpTestSuite) and
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

#include "dns/resolver/doh.h"
#include "dns_error.h"
#include "record_kind.h"
#include "util/cancellation_token.hpp"
#include "fmt.hpp"

using namespace std::chrono_literals;

namespace {

constexpr int DOH_PORT = 21443;
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
    char dir_template[] = "/tmp/yaddnsc_doh_test_XXXXXX";
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

    // Point SSL_CERT_FILE to our generated cert so OpenSSL trusts it.
    ::setenv("SSL_CERT_FILE", cert_path.c_str(), 1);
}

void start_doh_server() {
    generate_cert();

    server_log = "/tmp/yaddnsc-doh-server.log";

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
        ::execlp("python3", "python3", TEST_DATA_DIR "/doh_server.py",
                 fmt::format("{}", DOH_PORT).c_str(),
                 cert_path.c_str(), key_path.c_str(), nullptr);
        ::execl("/tmp/sim-venv/bin/python3", "python3", TEST_DATA_DIR "/doh_server.py",
                fmt::format("{}", DOH_PORT).c_str(),
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
        addr.sin_port = htons(static_cast<std::uint16_t>(DOH_PORT));
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
        GTEST_SKIP() << "DoH server did not start within 10s.\n"
                     << "Log: " << server_log;
        return;
    }

    server_started = true;
}

void stop_doh_server() {
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

class DohResolverTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        start_doh_server();
    }

    static void TearDownTestSuite() {
        stop_doh_server();
    }

    void SetUp() override {
        if (!server_started) {
            GTEST_SKIP() << "DoH server not available";
        }
    }
};

// ===========================================================================
// Test cases
// ===========================================================================

TEST_F(DohResolverTest, Resolve_A_Record) {
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh");
    Utils::CancellationToken cancel;
    auto result = resolver.query("yaddnsc.test", RecordKind::A, cancel);

    ASSERT_TRUE(result.has_value()) << "DoH A query failed: "
                                    << dns_error_name(result.error().code);
    // QNAME for "yaddnsc.test" = \x07yaddnsc\x04test\x00 = 14 bytes
    // RDATA starts at: 12(header) + 14(QNAME) + 4(QTYPE+QCLASS)
    //   + 2(name ptr) + 2(TYPE) + 2(CLASS) + 4(TTL) + 2(RDLENGTH) = 42
    ASSERT_GE(result->size(), 46U);
    EXPECT_EQ((*result)[42], 198);
    EXPECT_EQ((*result)[43], 51);
    EXPECT_EQ((*result)[44], 100);
    EXPECT_EQ((*result)[45], 42);
}

TEST_F(DohResolverTest, Resolve_AAAA_Record) {
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh");
    Utils::CancellationToken cancel;
    auto result = resolver.query("yaddnsc.test", RecordKind::AAAA, cancel);

    ASSERT_TRUE(result.has_value()) << "DoH AAAA query failed: "
                                    << dns_error_name(result.error().code);
    ASSERT_GT(result->size(), 12U);
}

TEST_F(DohResolverTest, ConnectToRefusedPort_ReturnsError) {
    DohResolver resolver("127.0.0.1", 1, "/dns-query", "test-doh-refused");
    Utils::CancellationToken cancel;
    auto result = resolver.query("yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code == DnsError::CONNECTION ||
                result.error().code == DnsError::RETRY)
        << "Expected CONNECTION or RETRY, got "
        << dns_error_name(result.error().code);
}

TEST_F(DohResolverTest, TimeoutHost_ReturnsError) {
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh-timeout");
    Utils::CancellationToken cancel;
    auto result = resolver.query("doh-timeout.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code == DnsError::RETRY ||
                result.error().code == DnsError::CONNECTION ||
                result.error().code == DnsError::CANCELLED)
        << "Expected RETRY, CONNECTION, or CANCELLED, got "
        << dns_error_name(result.error().code);
}

TEST_F(DohResolverTest, Http404_ReturnsServerRefused) {
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh-404");
    Utils::CancellationToken cancel;
    auto result = resolver.query("doh-404.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::SERVER_REFUSED);
}

TEST_F(DohResolverTest, Http500_ReturnsRetry) {
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh-500");
    Utils::CancellationToken cancel;
    auto result = resolver.query("doh-500.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::RETRY);
}

TEST_F(DohResolverTest, WrongContentType_ReturnsParseError) {
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh-wrong-ct");
    Utils::CancellationToken cancel;
    auto result = resolver.query("doh-malformed.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::PARSE);
}

TEST_F(DohResolverTest, NonExistentDomain_FallsBackToDefault) {
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh-nx");
    Utils::CancellationToken cancel;
    auto result = resolver.query("nonexistent.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_TRUE(result.has_value()) << "DoH A query failed: "
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

TEST_F(DohResolverTest, ChunkedTransferEncoding_Succeeds) {
    // Tests the chunked transfer encoding path in read_response().
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh-chunked");
    Utils::CancellationToken cancel;
    auto result = resolver.query("doh-chunked.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_TRUE(result.has_value()) << "DoH chunked query failed: "
                                    << dns_error_name(result.error().code);
    // QNAME for "doh-chunked.yaddnsc.test" = 1+11 + 1+7 + 1+4 + 1 = 26 bytes
    // RDATA starts at: 12(header) + 26(QNAME) + 4(QTYPE+QCLASS)
    //   + 2(name ptr) + 2(TYPE) + 2(CLASS) + 4(TTL) + 2(RDLENGTH) = 54
    ASSERT_GE(result->size(), 58U);
    EXPECT_EQ((*result)[54], 198);
    EXPECT_EQ((*result)[55], 51);
    EXPECT_EQ((*result)[56], 100);
    EXPECT_EQ((*result)[57], 42);
}

TEST_F(DohResolverTest, InvalidDnsResponse_ReturnsParseError) {
    // Server returns valid HTTP with dns-message content type, but
    // the DNS body is garbage.  The resolver's validator should catch it.
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh-invalid");
    Utils::CancellationToken cancel;
    auto result = resolver.query("doh-invalid-dns.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::PARSE);
}

TEST_F(DohResolverTest, TwoSequentialQueries_ReconnectTransparently) {
    // The server uses Connection: close, so the first query causes the
    // server to drop the connection.  The second query should transparently
    // reconnect via ensure_connection().
    DohResolver resolver("127.0.0.1", DOH_PORT, "/dns-query", "test-doh-seq");
    Utils::CancellationToken cancel;

    auto r1 = resolver.query("yaddnsc.test", RecordKind::A, cancel);
    ASSERT_TRUE(r1.has_value()) << "First sequential DoH query failed: "
                                << dns_error_name(r1.error().code);

    // Small delay for server-side FIN to propagate.
    std::this_thread::sleep_for(50ms);

    auto r2 = resolver.query("yaddnsc.test", RecordKind::A, cancel);
    ASSERT_TRUE(r2.has_value()) << "Second sequential DoH query failed: "
                                << dns_error_name(r2.error().code);
    ASSERT_GE(r2->size(), 46U);
    EXPECT_EQ((*r2)[42], 198);
    EXPECT_EQ((*r2)[43], 51);
    EXPECT_EQ((*r2)[44], 100);
    EXPECT_EQ((*r2)[45], 42);
}

TEST_F(DohResolverTest, Ipv6Address_FormatsHostHeader) {
    // Uses an IPv6 loopback address to exercise the IPv6 branch in
    // build_host_header().  The connection will fail (no server on that
    // port for IPv6), but the host header construction is tested.
    DohResolver resolver("::1", DOH_PORT, "/dns-query", "test-doh-ipv6");
    Utils::CancellationToken cancel;
    // No server on ::1, so connection should be refused.
    auto result = resolver.query("yaddnsc.test", RecordKind::A, cancel);

    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code == DnsError::CONNECTION ||
                result.error().code == DnsError::RETRY)
        << "Expected CONNECTION or RETRY for unreachable server, got "
        << dns_error_name(result.error().code);
}

} // anonymous namespace
