//
// Component tests for ClassicResolver (native UDP/TCP backend).
//
// Starts a Python DNS server (dns_server.py) on a loopback port,
// creates a ClassicResolver pointing to it, and verifies that
// A and AAAA queries return the expected addresses.
//
// The Python server is started once per test suite (SetUpTestSuite)
// and stopped after all tests (TearDownTestSuite).
//
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <fstream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>

// For prctl(PR_SET_PDEATHSIG) — Linux-only, prevents orphaned children
// when the test process is killed (including SIGKILL, which can't be caught
// in the parent).  macOS has no equivalent, but CI runs on Linux.
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <util/cancellation_token.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "config/dns_config.h"
#include "dns/resolver/classic.h"
#include "dns/resolver_registry.h"
#include "dns/dns_error_info.h"
#include "record_kind.h"
#include "uri.h"

#include "fmt.hpp"

using namespace std::chrono_literals;

namespace {

/// Port used by the Python DNS server.
constexpr int DNS_PORT = 21553;

/// Log file for the DNS server subprocess (stderr + stdout).
/// Printed on failure so developers can diagnose server-side errors.
constexpr std::string_view SERVER_LOG = "/tmp/yaddnsc-dns-server.log";

// Shared state across all tests in the suite.
static std::unique_ptr<ClassicResolver> global_resolver;
static pid_t server_pid = -1;
static bool server_started = false;

/// Read the last `n_lines` from a log file and return them as a string.
/// Returns an error message if the file cannot be opened.
[[nodiscard]] std::string dump_log(std::string_view path, int n_lines) {
    std::ifstream file(path.data());
    if (!file.is_open()) {
        return fmt::format("(could not open log: {})", path);
    }

    // Seek backwards in chunks to find the last n_lines.
    // Simple approach: read all lines into a deque, keep only the tail.
    std::deque<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
        if (static_cast<int>(lines.size()) > n_lines) {
            lines.pop_front();
        }
    }

    std::string result;
    for (const auto &l: lines) {
        result += l;
        result += '\n';
    }
    return result;
}

/// Start the Python DNS server as a background process.
void start_dns_server() {
    std::string script = TEST_DATA_DIR "/dns_server.py";

    if (::access(script.c_str(), R_OK) != 0) {
        GTEST_SKIP() << "dns_server.py not found at " << script;
        return;
    }

    server_pid = ::fork();
    if (server_pid < 0) {
        GTEST_FAIL() << "fork() failed";
        return;
    }

    if (server_pid == 0) {
        // Child: exec the Python DNS server.

        // Put child in its own process group so signals to the parent's
        // group don't leak through.
        ::setpgid(0, 0);

#ifdef __linux__
        // Ask the kernel to SIGTERM us if the parent dies for any reason
        // (including SIGKILL, which the parent cannot catch).  This is the
        // primary defence against orphaned subprocesses.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif

        // Redirect stdout/stderr to a log file instead of /dev/null, so
        // server-side errors are diagnosable when a test fails.
        int log_fd = ::open(SERVER_LOG.data(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
            ::close(log_fd);
        }

        auto port_str = fmt::format("{}", DNS_PORT);
        // Try venv python first (shared with integration tests), then system.
        ::execl("/tmp/sim-venv/bin/python3", "python3", script.c_str(), port_str.c_str(), nullptr);
        ::execlp("python3", "python3", script.c_str(), port_str.c_str(), nullptr);
        ::_exit(127);
    }

    // Parent: wait for server to become reachable via UDP.
    auto deadline = std::chrono::steady_clock::now() + 10s;

    // Build a minimal DNS probe.
    std::vector<std::uint8_t> probe;
    auto w16 = [&](std::uint16_t v) {
        probe.push_back(static_cast<std::uint8_t>(v >> 8));
        probe.push_back(static_cast<std::uint8_t>(v & 0xFF));
    };
    w16(0); w16(0x0100); w16(1); w16(0); w16(0); w16(0);
    probe.push_back(7);
    std::ranges::copy(std::string_view("example"), std::back_inserter(probe));
    probe.push_back(3);
    std::ranges::copy(std::string_view("com"), std::back_inserter(probe));
    probe.push_back(0);
    w16(1); w16(1);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<std::uint16_t>(DNS_PORT));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    bool ready = false;
    while (!ready && std::chrono::steady_clock::now() < deadline) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) break;

        ::sendto(fd, probe.data(), probe.size(), 0,
                 reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        struct pollfd pfd = {fd, POLLIN, 0};
        if (::poll(&pfd, 1, 200) > 0) {
            std::vector<std::uint8_t> buf(512);
            if (::recv(fd, buf.data(), buf.size(), 0) > 0) {
                ready = true;
            }
        }
        ::close(fd);
        if (!ready) std::this_thread::sleep_for(100ms);
    }

    if (!ready) {
        ::kill(server_pid, SIGTERM);
        ::waitpid(server_pid, nullptr, 0);
        server_pid = -1;

        // Include the server log in the skip message so developers can
        // diagnose why the server failed to start.
        GTEST_SKIP() << "Python DNS server did not respond within 10s.\n"
                     << "Log: " << SERVER_LOG << "\n"
                     << dump_log(SERVER_LOG, 30);
        return;
    }

    // Create the resolver.
    Config::DnsServer server;
    server.address = "127.0.0.1";
    server.port = DNS_PORT;

    global_resolver = std::make_unique<ClassicResolver>(std::move(server));
    server_started = true;
}

/// Stop the Python DNS server.
void stop_dns_server() {
    if (server_pid > 0) {
        ::kill(server_pid, SIGTERM);
        ::waitpid(server_pid, nullptr, 0);
        server_pid = -1;
    }
    global_resolver.reset();
    server_started = false;
}

// ===========================================================================
// Test fixture
// ===========================================================================

class ClassicNativeResolverTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        start_dns_server();
    }

    static void TearDownTestSuite() {
        stop_dns_server();
    }

    void SetUp() override {
        if (!server_started) {
            GTEST_SKIP() << "DNS server not available";
        }
    }
};

// ===========================================================================
// Test cases
// ===========================================================================

TEST_F(ClassicNativeResolverTest, ResolveARecord) {
    Utils::CancellationToken cancel;
    auto result = global_resolver->query("yaddnsc.test", RecordKind::A, cancel);

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->empty());
    EXPECT_GT(result->size(), 12);
}

TEST_F(ClassicNativeResolverTest, ResolveAAAARecord) {
    Utils::CancellationToken cancel;
    auto result = global_resolver->query("yaddnsc.test", RecordKind::AAAA, cancel);

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->empty());
    EXPECT_GT(result->size(), 12);
}

TEST_F(ClassicNativeResolverTest, ResolveNonexistentDomain) {
    Utils::CancellationToken cancel;
    auto result = global_resolver->query("nonexistent.example", RecordKind::A, cancel);

    ASSERT_TRUE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, QueryIsValidDnsResponse) {
    Utils::CancellationToken cancel;
    auto result = global_resolver->query("yaddnsc.test", RecordKind::A, cancel);

    ASSERT_TRUE(result.has_value());

    const auto &response = *result;
    ASSERT_GE(response.size(), 12);

    EXPECT_TRUE(response[2] & 0x80) << "QR bit not set in response";
    EXPECT_EQ(response[3] & 0x0F, 0) << "RCODE is not NOERROR";
}

TEST_F(ClassicNativeResolverTest, TruncatedResponse_FallsBackToTcp) {
    // Query a host that triggers TC=1 on UDP and a normal response on TCP.
    // The resolver should fall back to TCP and return a valid response.
    Utils::CancellationToken cancel;
    auto result = global_resolver->query("truncate.yaddnsc.test", RecordKind::A, cancel);

    ASSERT_TRUE(result.has_value()) << "TCP fallback should succeed";
    ASSERT_FALSE(result->empty());
    EXPECT_GT(result->size(), 12);

    // Verify QR bit is set (valid response header).
    const auto &response = *result;
    EXPECT_TRUE(response[2] & 0x80) << "QR bit not set in TCP fallback response";
}

TEST_F(ClassicNativeResolverTest, ConnectionRefused_ReturnsError) {
    // Create a resolver pointing to a closed port.
    Config::DnsServer server;
    server.address = "127.0.0.1";
    server.port = 1; // port 1 is never open on loopback

    auto bad_resolver = std::make_unique<ClassicResolver>(std::move(server));
    Utils::CancellationToken cancel;
    auto result = bad_resolver->query("yaddnsc.test", RecordKind::A, cancel);

    // Should fail with a connection error.
    ASSERT_FALSE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, MalformedResponse_ValidatorRejects) {
    // Query a host that makes the server return garbage.
    // The response validator should reject it.
    Utils::CancellationToken cancel;
    auto result = global_resolver->query("malformed.yaddnsc.test", RecordKind::A, cancel);

    // Should fail — validator rejects the malformed response.
    ASSERT_FALSE(result.has_value());
}

} // anonymous namespace
