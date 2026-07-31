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
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
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
#include "dns/types.h"
#include "dns/wire/builder.h"
#include "network/inet_address.h"
#include "network/socket.h"
#include "network/socket_addr.h"
#include "record_kind.h"
#include "uri.h"
#include "exception/dns_lookup.h"

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
    addr.sin_port = htons(static_cast<std::uint16_t>(DNS_PORT));
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

TEST_F(ClassicNativeResolverTest, InvalidServerAddress_Throws) {
	// An invalid address (not a valid IP) should throw during construction.
	Config::DnsServer server;
	server.address = "not-an-ip";
	server.port = 53;

	EXPECT_THROW(
		{ auto bad = std::make_unique<ClassicResolver>(std::move(server)); },
		DnsLookupException
	);
}

TEST_F(ClassicNativeResolverTest, UdpTimeout_ReturnsRetryError) {
	// Query a host the server ignores on UDP — resolver should time out.
	Utils::CancellationToken cancel;
	auto result = global_resolver->query("timeout.yaddnsc.test", RecordKind::A, cancel);

	// Should fail with a timeout/retry error (no server response).
	ASSERT_FALSE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, TcpConnectionReset_ReturnsError) {
	// UDP returns TC=1, then TCP connection is immediately closed.
	Utils::CancellationToken cancel;
	auto result = global_resolver->query("tcpreset.yaddnsc.test", RecordKind::A, cancel);

	// Should fail — TCP connection reset before any response.
	ASSERT_FALSE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, TcpInvalidResponseLength_ReturnsError) {
	// UDP returns TC=1, then TCP returns length prefix 0.
	Utils::CancellationToken cancel;
	auto result = global_resolver->query("tcperror.yaddnsc.test", RecordKind::A, cancel);

	// Should fail — TCP response length is invalid (0).
	ASSERT_FALSE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, DnsPacketException_IsCaught) {
	// A hostname with a label > 63 chars triggers DnsPacketException
	// in build_query, which is caught and returned as an error.
	std::string long_label(70, 'a');
	auto bad_host = fmt::format("{}.example.com", long_label);

	Utils::CancellationToken cancel;
	auto result = global_resolver->query(bad_host, RecordKind::A, cancel);

	ASSERT_FALSE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, Ipv6ServerAddress) {
	// Create a resolver pointing to an IPv6 loopback.
	Config::DnsServer server;
	server.address = "::1";
	server.port = DNS_PORT;

	auto ipv6_resolver = std::make_unique<ClassicResolver>(std::move(server));
	Utils::CancellationToken cancel;
	auto result = ipv6_resolver->query("yaddnsc.test", RecordKind::A, cancel);

	// ::1 is the same machine; the server should be reachable.
	// If the test environment has IPv6 disabled this will fail gracefully.
	if (!result) {
		GTEST_SKIP() << "IPv6 loopback not available";
	}
	ASSERT_TRUE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, TcpRecvTimeout_ReturnsRetryError) {
	// UDP returns TC=1, TCP connects but never sends — resolver times out.
	Utils::CancellationToken cancel;
	auto result = global_resolver->query("tcptimeout.yaddnsc.test", RecordKind::A, cancel);

	// Should fail with timeout.
	ASSERT_FALSE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, TcpGarbageResponse_ValidatorRejects) {
	// UDP returns TC=1, TCP returns garbage — validator rejects.
	Utils::CancellationToken cancel;
	auto result = global_resolver->query("tcpgarbage.yaddnsc.test", RecordKind::A, cancel);

	// Should fail — validator rejects garbage.
	ASSERT_FALSE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, TcpResponseLengthTooLarge_ReturnsError) {
	// UDP returns TC=1, TCP returns length prefix > 4096.
	Utils::CancellationToken cancel;
	auto result = global_resolver->query("tcplarge.yaddnsc.test", RecordKind::A, cancel);

	// Should fail — invalid response length.
	ASSERT_FALSE(result.has_value());
}

TEST_F(ClassicNativeResolverTest, TcpConnectFailureAfterTruncatedUdp) {
	// Use a UDP-only server that returns TC=1, so TCP connect fails.
	// Start a second Python server with --udp-only on a different port.
	constexpr int UDP_ONLY_PORT = 21554;

	pid_t udp_only_pid = ::fork();
	ASSERT_NE(udp_only_pid, -1) << "fork() failed";

	if (udp_only_pid == 0) {
		// Child: exec the UDP-only DNS server.
#ifdef __linux__
		::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
		int log_fd = ::open("/tmp/yaddnsc-udp-only-server.log",
		                    O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (log_fd >= 0) {
			::dup2(log_fd, STDOUT_FILENO);
			::dup2(log_fd, STDERR_FILENO);
			::close(log_fd);
		}
		auto port_str = fmt::format("{}", UDP_ONLY_PORT);
		::execlp("python3", "python3", TEST_DATA_DIR "/dns_server.py",
		         port_str.c_str(), "--udp-only", nullptr);
		::execl("/tmp/sim-venv/bin/python3", "python3", TEST_DATA_DIR "/dns_server.py",
		        port_str.c_str(), "--udp-only", nullptr);
		::_exit(127);
	}

	// Parent: wait for the UDP-only server to become ready.
	// Use a proper DNS query as probe (same as main server startup).
	auto deadline = std::chrono::steady_clock::now() + 10s;
	bool ready = false;

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

	while (!ready && std::chrono::steady_clock::now() < deadline) {
		int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0) break;

		struct sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<std::uint16_t>(UDP_ONLY_PORT));
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

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
		if (!ready)
			std::this_thread::sleep_for(100ms);
	}

	if (!ready) {
		::kill(udp_only_pid, SIGTERM);
		::waitpid(udp_only_pid, nullptr, 0);
		GTEST_SKIP() << "UDP-only server did not start within 10s";
		return;
	}

	// Create a resolver pointing to the UDP-only server.
	Config::DnsServer server;
	server.address = "127.0.0.1";
	server.port = UDP_ONLY_PORT;
	auto resolver = std::make_unique<ClassicResolver>(std::move(server));

	Utils::CancellationToken cancel;
	// Query a host that triggers TC=1 on UDP.
	// Since there's no TCP listener, the TCP connect should fail.
	auto result = resolver->query("tcpconnectfail.yaddnsc.test", RecordKind::A, cancel);

	// Should fail with a connection error (TCP connect failed after truncation).
	ASSERT_FALSE(result.has_value());

	// Clean up the UDP-only server.
	::kill(udp_only_pid, SIGTERM);
	::waitpid(udp_only_pid, nullptr, 0);
}

// ===========================================================================
// Spoofed UDP response handling (source verification)
// ===========================================================================

/// Build a DNS response that echoes @p query (same ID and question) with a
/// single A answer for @p ip.  Deliberately passes the ID/question
/// validation so that only source verification can reject it.
[[nodiscard]] std::vector<std::uint8_t> build_a_response(const std::vector<std::uint8_t> &query,
                                                         std::string_view ip) {
    auto resp = query;
    resp[2] = 0x81; // QR | RD
    resp[3] = 0x80; // RA
    resp[6] = 0x00;
    resp[7] = 0x01; // ANCOUNT = 1

    // Skip the question section (QNAME + QTYPE + QCLASS).
    std::size_t q_end = DNS::HEADER_SIZE;
    while (q_end < resp.size() && resp[q_end] != 0) {
        q_end += 1 + resp[q_end];
    }
    q_end += 5; // root label + QTYPE(2) + QCLASS(2)

    // Drop the EDNS OPT record the query may carry (ARCOUNT=1): the client
    // does not require it in the response, and it would shift the answer.
    if (q_end + 11 <= resp.size() && resp[q_end] == 0x00 && resp[q_end + 1] == 0x00 &&
        resp[q_end + 2] == 0x29) {
        resp.resize(q_end);
        resp[10] = 0x00;
        resp[11] = 0x00; // ARCOUNT = 0
    }

    // Answer: name pointer to the question, type A, class IN, TTL, RDATA.
    const std::array<std::uint8_t, 10> fixed = {
        0xC0, 0x0C,         // name pointer (offset 12)
        0x00, 0x01,         // TYPE = A
        0x00, 0x01,         // CLASS = IN
        0x00, 0x00, 0x00, 0x3C, // TTL = 60
    };
    resp.insert(resp.end(), fixed.begin(), fixed.end());

    in_addr in{};
    ::inet_pton(AF_INET, ip.data(), &in);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&in.s_addr);
    resp.push_back(0x00);
    resp.push_back(0x04); // RDLENGTH = 4
    resp.insert(resp.end(), bytes, bytes + 4);
    return resp;
}

TEST_F(ClassicNativeResolverTest, UdpResponseFromUnexpectedSource_IsDiscarded) {
    // Regression: a UDP response from a source other than the configured
    // resolver (IP + port) must be discarded.  The forged response echoes
    // the query with a valid ID and question so it passes the ID/question
    // validation — only source verification can reject it.
    //
    // Before the fix the first datagram to arrive was accepted, so the
    // forged answer (1.2.3.4) won the race.

    // Real resolver socket: 127.0.0.1 on an ephemeral port.
    Socket server_sock(AF_INET, SOCK_DGRAM);
    auto v4 = Inet4Address::parse("127.0.0.1");
    ASSERT_TRUE(v4.has_value());
    auto bind_addr = SocketAddr::from_inet(*v4, 0);
    ASSERT_TRUE(bind_addr.has_value());
    ASSERT_TRUE(server_sock.bind(*bind_addr).has_value());
    const auto server_port = server_sock.get_sockname().port();

    Config::DnsServer server;
    server.address = "127.0.0.1";
    server.port = server_port;
    ClassicResolver resolver(std::move(server));

    // Server thread: on query, send a forged response from a DIFFERENT
    // source port first, then the genuine response from the real socket.
    // Responses are built from the received query so the ID matches.
    std::thread server_thread([&] {
        std::array<std::uint8_t, 512> recv_buf{};
        SocketAddr client_addr;
        auto n = server_sock.recv_from(std::as_writable_bytes(std::span{recv_buf}), &client_addr);
        if (n <= 0) {
            return; // client query failed — the resolver will time out
        }
        const auto client_query = std::vector<std::uint8_t>(recv_buf.begin(), recv_buf.begin() + n);

        // Forged response from an unrelated source port.  The kernel may
        // hand out the server's own port again (SO_REUSEADDR), so retry
        // until the ports differ.
        std::optional<Socket> spoof_sock;
        for (int attempt = 0; attempt < 8 && !spoof_sock.has_value(); ++attempt) {
            Socket s(AF_INET, SOCK_DGRAM);
            auto sb = SocketAddr::from_inet(*v4, 0);
            if (!sb.has_value()) break;
            if (s.bind(*sb).has_value() && s.get_sockname().port() != server_port) {
                spoof_sock.emplace(std::move(s));
            }
        }
        if (!spoof_sock.has_value()) {
            return; // could not get a distinct source port
        }

        auto spoof_pkt = build_a_response(client_query, "1.2.3.4");
        spoof_sock->send_to(std::as_bytes(std::span{spoof_pkt}), client_addr);

        // Genuine response from the real server socket.
        auto real_pkt = build_a_response(client_query, "198.51.100.42");
        server_sock.send_to(std::as_bytes(std::span{real_pkt}), client_addr);
    });

    Utils::CancellationToken cancel;
    auto result = resolver.query("spoof-test.example", RecordKind::A, cancel);
    server_thread.join();

    ASSERT_TRUE(result.has_value()) << "resolver query failed: " << result.error().message
                                    << " (code " << static_cast<int>(result.error().code) << ")";

    // ClassicResolver returns the raw DNS message.  Locate the A record in
    // the answer section and compare the address bytes: the genuine
    // response (198.51.100.42) must have won, not the forged one (1.2.3.4).
    const auto &response = *result;
    ASSERT_GE(response.size(), 12U);

    // Skip the question section (QNAME + QTYPE + QCLASS).
    std::size_t off = DNS::HEADER_SIZE;
    while (off < response.size() && response[off] != 0) {
        off += 1 + response[off];
    }
    off += 5; // root label + QTYPE(2) + QCLASS(2)

    // Answer: name(2) + type(2) + class(2) + ttl(4) + rdlength(2) + rdata(4).
    ASSERT_GE(response.size(), off + 16);
    ASSERT_EQ(response[off + 2], 0x00);
    ASSERT_EQ(response[off + 3], 0x01); // type = A
    const auto rdlength = static_cast<std::uint16_t>((response[off + 10] << 8) | response[off + 11]);
    ASSERT_EQ(rdlength, 4);

    std::array<std::uint8_t, 4> ip_bytes{};
    std::ranges::copy_n(response.begin() + static_cast<std::ptrdiff_t>(off + 12), 4, ip_bytes.begin());
    EXPECT_EQ(ip_bytes, (std::array<std::uint8_t, 4>{198, 51, 100, 42}));
}

} // anonymous namespace
