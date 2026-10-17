//
// Component tests for the coroutine HTTP client and DNS subsystem.
//
// Real I/O: an in-process HTTP/1.1 server for the plain-TCP paths, and the
// shared Python servers (dns_server.py / doh_server.py / dot_server.py) for the
// classic, DoH and DoT paths. Each Python test skips cleanly when python3 or the
// openssl CLI is unavailable, exactly like the legacy component tests.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only inside coroutine bodies.
//

#include <array>
#include <chrono>
#include <compare>
#include <coroutine>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <expected>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "coro/coro.h"
#include "domain/config/dns_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/dispatcher.h"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/policy.h"
#include "infrastructure/dns/resolver/classic.h"
#include "infrastructure/dns/resolver/doh.h"
#include "infrastructure/dns/resolver/dot.h"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/http/error.h"
#include "infrastructure/http/persistent_client.h"
#include "infrastructure/http/transport.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/tls/context.h"
#include "infrastructure/network/transport/datagram.h"
#include "infrastructure/network/transport/udp_socket.h"
#include "support/fmt.hpp"
#include "support/util/fd.hpp"
#include "yaddnsc/util/format.hpp"

namespace net {
struct TlsTrustOptions;
}  // namespace net

namespace {

using namespace std::chrono_literals;

constexpr int CLASSIC_PORT = 21680;
constexpr int DOH_PORT = 21681;
constexpr int DOT_PORT = 21682;
constexpr int DOT_TIMEOUT_PORT = 21683;
constexpr int BOOTSTRAP_FIRST_PORT = 21684;
constexpr int BOOTSTRAP_SECOND_PORT = 21685;

[[nodiscard]] domain::InetAddress loopback_v4() {
    const auto address = domain::InetAddress::parse("127.0.0.1");
    EXPECT_TRUE(address.has_value());
    return address.value_or(domain::InetAddress{});
}

/// Build the off-loop trust context a resolver's trust options ask for.
[[nodiscard]] std::shared_ptr<const net::TlsContext> client_context(const net::TlsTrustOptions& options) {
    auto context = net::TlsContext::create(options);
    EXPECT_TRUE(context.has_value()) << "TlsContext::create failed";
    return context.value_or(nullptr);
}

/// Run a task on a fresh loop with the system clock.
template<typename T>
T run_task(coro::Task<T> task) {
    coro::Loop loop;
    return coro::run(loop, std::move(task));
}

/// In-process HTTP/1.1 server: serves `max_requests` requests on one
/// connection, the first with chunked framing and the rest with Content-Length.
class LoopbackHttpServer {
public:
    explicit LoopbackHttpServer(const int max_requests) {
        listener_.reset(::socket(AF_INET, SOCK_STREAM, 0));
        const int enabled = 1;
        ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        EXPECT_EQ(::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0)
            << "cannot bind the loopback listener";

        socklen_t length = sizeof(address);
        ::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        ::listen(listener_.get(), 4);
        thread_ = std::jthread([this, max_requests] { serve(max_requests); });
    }

    ~LoopbackHttpServer() {
        if (listener_) {
            const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
            if (wake >= 0) {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(port_);
                EXPECT_EQ(::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
                ::close(wake);
            }
            listener_.reset();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    [[nodiscard]] static bool write_all(const int fd, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t wrote = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (wrote <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    void serve(const int max_requests) {
        const int raw = ::accept(listener_.get(), nullptr, nullptr);
        if (raw < 0) {
            return;
        }
        Utils::UniqueFd connection{raw};
        std::string buffer;
        for (int request = 0; request < max_requests; ++request) {
            while (buffer.find("\r\n\r\n") == std::string::npos) {
                std::array<char, 1024> chunk{};
                const ssize_t got = ::recv(connection.get(), chunk.data(), chunk.size(), 0);
                if (got <= 0) {
                    return;
                }
                buffer.append(chunk.data(), static_cast<std::size_t>(got));
            }
            buffer.erase(0, buffer.find("\r\n\r\n") + 4);  // body-less requests

            const std::string response = request == 0 ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                                        "5\r\nfirst\r\n0\r\n\r\n"
                                                      : "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond";
            if (!write_all(connection.get(), response)) {
                return;
            }
        }
    }

    Utils::UniqueFd listener_;
    std::uint16_t port_{0};
    std::jthread thread_;
};

/// A forked Python server, killed and reaped on destruction.
class PythonServer {
public:
    PythonServer() = default;

    /// Fork and exec the server. Returns false when python3/the script is not
    /// usable, so the caller can GTEST_SKIP() itself (a constructor cannot).
    [[nodiscard]] bool start(const std::string& script, const std::vector<std::string>& args) {
        const std::string path = std::string{TEST_DATA_DIR} + "/" + script;
        if (::access(path.c_str(), R_OK) != 0) {
            return false;
        }
        pid_ = ::fork();
        if (pid_ < 0) {
            return false;
        }
        if (pid_ == 0) {
            ::setpgid(0, 0);
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>("python3"));
            argv.push_back(const_cast<char*>(path.c_str()));
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);
            ::execvp("python3", argv.data());
            ::_exit(127);
        }
        return true;
    }

    ~PythonServer() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            ::waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }

    PythonServer(const PythonServer&) = delete;
    PythonServer& operator=(const PythonServer&) = delete;

    /// Probe a TCP listener until it accepts or the budget runs out. The
    /// budget is generous: fork/exec of python3 plus interpreter startup can
    /// take seconds on a loaded CI runner.
    [[nodiscard]] static bool wait_for_tcp(const std::uint16_t port, const std::chrono::milliseconds budget = 30s) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
            if (probe < 0) {
                return false;
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            const bool ready = ::connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
            ::close(probe);
            if (ready) {
                return true;
            }
            std::this_thread::sleep_for(25ms);
        }
        return false;
    }

private:
    pid_t pid_{-1};
};

/// Create a throwaway self-signed certificate for an IP-SAN endpoint.
[[nodiscard]] std::optional<std::string> make_self_signed_cert(std::string& directory) {
    char path_template[] = "/tmp/yaddnsc_net_coro_io_XXXXXX";
    auto* created = ::mkdtemp(path_template);
    if (created == nullptr) {
        return std::nullopt;
    }
    directory = created;
    std::string cert = directory + "/cert.pem";
    const std::string key = directory + "/key.pem";
    const std::string command = "openssl req -x509 -newkey rsa:2048 -keyout " + key + " -out " + cert +
                                " -days 1 -nodes -subj /CN=127.0.0.1 "
                                "-addext subjectAltName=IP:127.0.0.1 2>/dev/null";
    if (::system(command.c_str()) != 0) {
        return std::nullopt;
    }
    return cert;
}

// ---------------------------------------------------------------------------
// HTTP over a real TCP connection
// ---------------------------------------------------------------------------

TEST(NetCoroHttp, persistentClient_twoExchanges_reuseOneConnection) {
    LoopbackHttpServer server{2};
    http::Options options;
    http::PersistentClient client{fmt::format("http://127.0.0.1:{}", server.port()), options};

    auto sent = [&]() -> coro::Task<std::pair<std::string, std::string>> {
        http::Request request;
        const auto first = co_await client.exchange("/first", request);
        const auto second = co_await client.exchange("/second", request);
        co_return std::pair{first ? std::string(first->text()) : std::string{"<error>"},
                            second ? std::string(second->text()) : std::string{"<error>"}};
    };

    const auto [first_body, second_body] = run_task(sent());

    EXPECT_EQ(first_body, "first");    // chunked framing
    EXPECT_EQ(second_body, "second");  // Content-Length framing on the same connection
}

// ---------------------------------------------------------------------------
// classic DNS against the shared Python server
// ---------------------------------------------------------------------------

TEST(NetCoroDns, classic_resolvesOverUdpAndOverTcpOnTruncation) {
    PythonServer server;
    if (!server.start("dns_server.py", {std::to_string(CLASSIC_PORT)})) {
        GTEST_SKIP() << "dns_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(CLASSIC_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }

    dns::ClassicResolver resolver{loopback_v4(), CLASSIC_PORT};

    auto resolve = [&]() -> coro::Task<std::pair<bool, bool>> {
        auto normal = co_await resolver.query("yaddnsc.test", domain::RecordKind::A);
        auto truncated = co_await resolver.query("truncate.yaddnsc.test", domain::RecordKind::A);
        co_return std::pair{normal.has_value() && !normal->empty(), truncated.has_value() && !truncated->empty()};
    };

    const auto [normal, truncated] = run_task(resolve());
    EXPECT_TRUE(normal);     // answered over UDP
    EXPECT_TRUE(truncated);  // TC set on UDP, answered over TCP
}

TEST(NetCoroDns, classic_acceptsTcpAnswersBeyondTheLegacyCap) {
    // tcplarge.yaddnsc.test: UDP gets TC=1, then TCP answers with a valid
    // 5000-byte message — beyond the legacy 4096 classic cap but within the
    // 65535 the two-byte prefix allows (RFC 1035 §4.2.2).
    PythonServer server;
    if (!server.start("dns_server.py", {std::to_string(CLASSIC_PORT)})) {
        GTEST_SKIP() << "dns_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(CLASSIC_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }

    dns::ClassicResolver resolver{loopback_v4(), CLASSIC_PORT};

    const auto result = run_task([&]() -> coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> {
        co_return co_await resolver.query("tcplarge.yaddnsc.test", domain::RecordKind::A);
    }());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 5000U);
}

TEST(NetCoroDns, classic_rejectsTcpAnswersBeyondTheAcceptedBound) {
    // tcphuge.yaddnsc.test: UDP gets TC=1, then TCP announces 20000 bytes —
    // past MAX_RESPONSE_SIZE. The resolver must reject the endpoint as
    // retryable right after the prefix instead of buffering the body, so the
    // dispatcher can fall over to the next server.
    PythonServer server;
    if (!server.start("dns_server.py", {std::to_string(CLASSIC_PORT)})) {
        GTEST_SKIP() << "dns_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(CLASSIC_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }

    dns::ClassicResolver resolver{loopback_v4(), CLASSIC_PORT};

    const auto result = run_task([&]() -> coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> {
        co_return co_await resolver.query("tcphuge.yaddnsc.test", domain::RecordKind::A);
    }());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
}

TEST(NetCoroDns, classic_oversizedUdpDatagramWithoutTc_FallsBackToTcp) {
    // udplarge.yaddnsc.test: UDP answers 5000 bytes WITHOUT the TC bit, so the
    // kernel truncates the datagram — only a client that detects that
    // (MSG_TRUNC) falls back to TCP instead of failing to parse.
    PythonServer server;
    if (!server.start("dns_server.py", {std::to_string(CLASSIC_PORT)})) {
        GTEST_SKIP() << "dns_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(CLASSIC_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }

    dns::ClassicResolver resolver{loopback_v4(), CLASSIC_PORT};

    const auto result = run_task([&]() -> coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> {
        co_return co_await resolver.query("udplarge.yaddnsc.test", domain::RecordKind::A);
    }());

    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
}

// ---------------------------------------------------------------------------
// DoH and DoT against the shared Python servers
// ---------------------------------------------------------------------------

TEST(NetCoroDns, doh_resolvesThroughThePersistentHttpSession) {
    std::string directory;
    const auto cert = make_self_signed_cert(directory);
    if (!cert.has_value()) {
        GTEST_SKIP() << "failed to generate a throwaway certificate";
    }
    PythonServer server;
    if (!server.start("doh_server.py", {std::to_string(DOH_PORT), *cert, directory + "/key.pem"})) {
        GTEST_SKIP() << "doh_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(DOH_PORT)) {
        GTEST_SKIP() << "doh_server.py did not start";
    }

    http::Options options;
    options.tls_context = client_context({.verify_peer = false});
    dns::DohResolver resolver{fmt::format("https://127.0.0.1:{}/dns-query", DOH_PORT), options};

    auto resolve = [&]() -> coro::Task<std::pair<bool, bool>> {
        auto first = co_await resolver.query("yaddnsc.test", domain::RecordKind::A);
        auto second = co_await resolver.query("yaddnsc.test", domain::RecordKind::AAAA);
        co_return std::pair{first.has_value() && !first->empty(), second.has_value() && !second->empty()};
    };

    const auto [ipv4_ok, ipv6_ok] = run_task(resolve());
    EXPECT_TRUE(ipv4_ok);  // A answered over HTTPS
    EXPECT_TRUE(ipv6_ok);  // AAAA answered on the reused TLS session
}

TEST(NetCoroDns, doh_enforcesResponseSemantics) {
    std::string directory;
    const auto cert = make_self_signed_cert(directory);
    if (!cert.has_value()) {
        GTEST_SKIP() << "failed to generate a throwaway certificate";
    }
    PythonServer server;
    if (!server.start("doh_server.py", {std::to_string(DOH_PORT), *cert, directory + "/key.pem"})) {
        GTEST_SKIP() << "doh_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(DOH_PORT)) {
        GTEST_SKIP() << "doh_server.py did not start";
    }

    http::Options options;
    options.tls_context = client_context({.verify_peer = false});
    dns::DohResolver resolver{fmt::format("https://127.0.0.1:{}/dns-query", DOH_PORT), options};

    using QueryResult = std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>;
    auto resolve = [&]() -> coro::Task<std::array<QueryResult, 4>> {
        std::array<QueryResult, 4> results;
        results[0] = co_await resolver.query("doh-redirect.yaddnsc.test", domain::RecordKind::A);
        results[1] = co_await resolver.query("doh-201.yaddnsc.test", domain::RecordKind::A);
        results[2] = co_await resolver.query("doh-tc.yaddnsc.test", domain::RecordKind::A);
        results[3] = co_await resolver.query("doh-malformed.yaddnsc.test", domain::RecordKind::A);
        co_return results;
    };

    const auto results = run_task(resolve());

    // RFC 8484 §3: the redirect back to this same endpoint is never followed.
    ASSERT_FALSE(results[0].has_value());
    EXPECT_EQ(results[0].error().code, domain::DnsError::SERVER_REFUSED);
    // RFC 8484 §4.2.1: any 2xx status carries a usable answer, not only 200.
    EXPECT_TRUE(results[1].has_value());
    // A TC answer has no TCP fallback in DoH: transient, so the dispatcher can
    // fail over to another backend instead of parsing partial data.
    ASSERT_FALSE(results[2].has_value());
    EXPECT_EQ(results[2].error().code, domain::DnsError::PARSE);
    // A non-DNS Content-Type is only this endpoint's failure.
    ASSERT_FALSE(results[3].has_value());
    EXPECT_EQ(results[3].error().code, domain::DnsError::PARSE);

    PythonServer classic;
    if (!classic.start("dns_server.py", {std::to_string(CLASSIC_PORT)}) || !PythonServer::wait_for_tcp(CLASSIC_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }
    std::vector<std::unique_ptr<dns::Resolver>> resolvers;
    resolvers.push_back(
        std::make_unique<dns::DohResolver>(fmt::format("https://127.0.0.1:{}/dns-query", DOH_PORT), options));
    resolvers.push_back(std::make_unique<dns::ClassicResolver>(loopback_v4(), CLASSIC_PORT));
    dns::Dispatcher dispatcher{std::move(resolvers), dns::Strategy::FALLBACK};
    const auto fallback = run_task(dispatcher.resolve("doh-malformed.yaddnsc.test", domain::RecordKind::A));
    ASSERT_TRUE(fallback.has_value()) << fallback.error().message;
    EXPECT_EQ(*fallback, (std::vector<std::string>{"198.51.100.1"}));
}

TEST(NetCoroDns, dot_resolvesOverTlsWithPadding) {
    std::string directory;
    const auto cert = make_self_signed_cert(directory);
    if (!cert.has_value()) {
        GTEST_SKIP() << "failed to generate a throwaway certificate";
    }
    PythonServer server;
    if (!server.start("dot_server.py", {std::to_string(DOT_PORT), *cert, directory + "/key.pem"})) {
        GTEST_SKIP() << "dot_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(DOT_PORT)) {
        GTEST_SKIP() << "dot_server.py did not start";
    }

    dns::EndpointOptions options;
    options.tls_context = client_context({.verify_peer = false});
    dns::DotResolver resolver{"127.0.0.1", DOT_PORT, options};

    using QueryResult = std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>;
    auto resolve = [&]() -> coro::Task<std::array<QueryResult, 3>> {
        std::array<QueryResult, 3> results;
        results[0] = co_await resolver.query("yaddnsc.test", domain::RecordKind::A);
        results[1] = co_await resolver.query("dot-tc.yaddnsc.test", domain::RecordKind::A);
        results[2] = co_await resolver.query("yaddnsc.test", domain::RecordKind::A);
        co_return results;
    };

    const auto results = run_task(resolve());
    EXPECT_TRUE(results[0].has_value());  // padded query answered
    ASSERT_FALSE(results[1].has_value());
    EXPECT_EQ(results[1].error().code, domain::DnsError::PARSE);
    EXPECT_TRUE(results[2].has_value());  // next query still works after the rejected answer
}

TEST(NetCoroDns, dot_peerClosesWithoutAnswering_FailsCleanly) {
    std::string directory;
    const auto cert = make_self_signed_cert(directory);
    if (!cert.has_value()) {
        GTEST_SKIP() << "failed to generate a throwaway certificate";
    }
    // dot_server.py accepts this name and then closes the connection without an
    // answer: the resolver must report a connection failure, not hang.
    PythonServer server;
    if (!server.start("dot_server.py", {std::to_string(DOT_TIMEOUT_PORT), *cert, directory + "/key.pem"})) {
        GTEST_SKIP() << "dot_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(DOT_TIMEOUT_PORT)) {
        GTEST_SKIP() << "dot_server.py did not start";
    }

    dns::EndpointOptions options;
    options.tls_context = client_context({.verify_peer = false});
    dns::DotResolver resolver{"127.0.0.1", DOT_TIMEOUT_PORT, options};

    const auto result = run_task([&]() -> coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> {
        co_return co_await resolver.query("dot-timeout.yaddnsc.test", domain::RecordKind::A);
    }());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONNECTION);
}

TEST(NetCoroDns, doh_scopeTimeout_abortsTheQuery) {
    std::string directory;
    const auto cert = make_self_signed_cert(directory);
    if (!cert.has_value()) {
        GTEST_SKIP() << "failed to generate a throwaway certificate";
    }
    PythonServer server;
    if (!server.start("doh_server.py", {std::to_string(DOH_PORT), *cert, directory + "/key.pem"})) {
        GTEST_SKIP() << "doh_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(DOH_PORT)) {
        GTEST_SKIP() << "doh_server.py did not start";
    }

    http::Options options;
    options.tls_context = client_context({.verify_peer = false});
    dns::DohResolver resolver{fmt::format("https://127.0.0.1:{}/dns-query", DOH_PORT), options};

    // doh_server.py accepts this name and then never answers, so only the
    // caller's cancel scope can end the wait — the transport has no timeout.
    bool timed_out = false;
    std::optional<domain::DnsErrorInfo> error;
    run_task([&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(300ms, [&]() -> coro::Task<void> {
            auto result = co_await resolver.query("doh-timeout.yaddnsc.test", domain::RecordKind::A);
            if (!result) {
                error = result.error();
            }
            co_return;
        });
        timed_out = outcome.timed_out;
        co_return;
    }());

    EXPECT_TRUE(timed_out);
    EXPECT_FALSE(error.has_value());  // cancellation bypasses the recoverable error channel
}

// ---------------------------------------------------------------------------
// dispatcher over real backends
// ---------------------------------------------------------------------------

TEST(NetCoroDns, dispatcher_concurrentRaceAgainstRealBackends) {
    PythonServer server;
    if (!server.start("dns_server.py", {std::to_string(CLASSIC_PORT)})) {
        GTEST_SKIP() << "dns_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(CLASSIC_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }

    // One backend that cannot answer (a closed port) racing one that can: the
    // concurrent strategy must still resolve, because scope exit joins both.
    std::vector<std::unique_ptr<dns::Resolver>> resolvers;
    resolvers.push_back(std::make_unique<dns::ClassicResolver>(loopback_v4(), 1));
    resolvers.push_back(std::make_unique<dns::ClassicResolver>(loopback_v4(), CLASSIC_PORT));
    dns::Dispatcher dispatcher{std::move(resolvers), dns::Strategy::CONCURRENT};

    const auto result = run_task([&]() -> coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> {
        co_return co_await dispatcher.resolve("yaddnsc.test", domain::RecordKind::A);
    }());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::string>{"198.51.100.42"}));
}

// ---------------------------------------------------------------------------
// bootstrap resolution across servers
// ---------------------------------------------------------------------------

TEST(NetCoroDns, bootstrap_nodataAnswerFallsThroughToTheNextServer) {
    // The first server answers NODATA (NOERROR, empty answers) for the name;
    // the second instance holds the record.
    PythonServer first;
    if (!first.start("dns_server.py", {std::to_string(BOOTSTRAP_FIRST_PORT)})) {
        GTEST_SKIP() << "dns_server.py could not be started";
    }
    PythonServer second;
    if (!second.start("dns_server.py", {std::to_string(BOOTSTRAP_SECOND_PORT),
                                        R"(--records={"nodata.yaddnsc.test":{"A":"198.51.100.77"}})"})) {
        GTEST_SKIP() << "dns_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(BOOTSTRAP_FIRST_PORT) || !PythonServer::wait_for_tcp(BOOTSTRAP_SECOND_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }

    const std::vector<domain::DnsServer> servers{{"127.0.0.1", static_cast<std::uint16_t>(BOOTSTRAP_FIRST_PORT)},
                                                 {"127.0.0.1", static_cast<std::uint16_t>(BOOTSTRAP_SECOND_PORT)}};
    http::Options options;
    options.resolve = dns::make_bootstrap_resolver(servers);
    options.address_family = domain::AddressFamily::IPV4;
    const auto result = run_task(http::resolve_host("nodata.yaddnsc.test", options));
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(result->front().to_string(), "198.51.100.77");
}

TEST(NetCoroDns, bootstrap_configErrorSurvivesNxdomainButDoesNotHideSuccess) {
    PythonServer server;
    if (!server.start("dns_server.py", {std::to_string(BOOTSTRAP_FIRST_PORT)}) ||
        !PythonServer::wait_for_tcp(BOOTSTRAP_FIRST_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }

    const domain::DnsServer valid{"127.0.0.1", static_cast<std::uint16_t>(BOOTSTRAP_FIRST_PORT)};
    const domain::DnsServer invalid{"not-an-ip", 53};
    const std::vector<std::vector<domain::DnsServer>> orders{{invalid, valid}, {valid, invalid}};
    for (const auto& servers : orders) {
        // This server NXDOMAINs AAAA for an unknown name. CONFIG must win
        // regardless of whether it occurs before or after that answer.
        const auto result =
            run_task(dns::bootstrap_resolve("unknown.yaddnsc.test", domain::AddressFamily::IPV6, servers));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
        EXPECT_NE(result.error().message.find("IP literal"), std::string::npos);

        const auto success = run_task(dns::bootstrap_resolve("yaddnsc.test", domain::AddressFamily::IPV6, servers));
        ASSERT_TRUE(success.has_value()) << success.error().message;
        ASSERT_EQ(success->size(), 1U);
        EXPECT_EQ(success->front().to_string(), "2001:db8::42");
    }
}

TEST(NetCoroDns, bootstrap_nxdomainFromOneServerStillTriesTheNext) {
    // The first server NXDOMAINs the unknown name; the second holds its AAAA
    // record. NXDOMAIN is authoritative for one server, not for the search.
    PythonServer first;
    if (!first.start("dns_server.py", {std::to_string(BOOTSTRAP_FIRST_PORT)})) {
        GTEST_SKIP() << "dns_server.py could not be started";
    }
    PythonServer second;
    if (!second.start("dns_server.py", {std::to_string(BOOTSTRAP_SECOND_PORT),
                                        R"(--records={"v6fallback.yaddnsc.test":{"AAAA":"2001:db8::77"}})"})) {
        GTEST_SKIP() << "dns_server.py could not be started";
    }
    if (!PythonServer::wait_for_tcp(BOOTSTRAP_FIRST_PORT) || !PythonServer::wait_for_tcp(BOOTSTRAP_SECOND_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }

    const std::vector<domain::DnsServer> servers{{"127.0.0.1", static_cast<std::uint16_t>(BOOTSTRAP_FIRST_PORT)},
                                                 {"127.0.0.1", static_cast<std::uint16_t>(BOOTSTRAP_SECOND_PORT)}};
    auto resolve = [&]() -> coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> {
        co_return co_await dns::bootstrap_resolve("v6fallback.yaddnsc.test", domain::AddressFamily::IPV6, servers);
    };

    const auto result = run_task(resolve());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(result->front().to_string(), "2001:db8::77");
}

enum class UdpReplyCase { STRAY_THEN_VALID, MALFORMED, LATE_TC };

coro::Task<void> answer_udp(net::UdpSocket& server, const UdpReplyCase kind) {
    std::array<std::uint8_t, 4096> buffer{};
    const auto received = co_await server.recv_from(buffer);
    EXPECT_TRUE(received.has_value());
    if (!received)
        co_return;
    std::vector<std::uint8_t> reply(buffer.begin(), buffer.begin() + received->size);
    reply[2] |= 0x80;
    if (kind == UdpReplyCase::MALFORMED) {
        reply.resize(5);  // Matching ID, incomplete header.
    } else if (kind == UdpReplyCase::LATE_TC) {
        // The TC reply must land inside (attempt_budget - wire_budget,
        // wire_budget) = (500ms, 1000ms): late enough that the outer attempt
        // budget fires while the TCP fallback is still waiting (so the error
        // names the fallback), early enough that UDP does not time out first.
        // 750ms centres that window.
        co_await coro::sleep_for(750ms);
        reply[2] |= 2;
    } else {
        auto stray = reply;
        stray[1] ^= 1;
        EXPECT_TRUE((co_await server.send_to(received->from, received->port, stray)).has_value());
        stray = reply;
        stray[13] = 'y';
        EXPECT_TRUE((co_await server.send_to(received->from, received->port, stray)).has_value());
    }
    EXPECT_TRUE((co_await server.send_to(received->from, received->port, reply)).has_value());
}

TEST(NetCoroDns, udpDistinguishesInvalidLocalMalformedRemoteAndStrayPackets) {
    const auto query = dns::QueryBuilder{}.add_question("x.test", dns::RecordType::A).build();
    for (const auto kind : {UdpReplyCase::MALFORMED, UdpReplyCase::STRAY_THEN_VALID}) {
        net::UdpSocket server;
        ASSERT_TRUE(server.bind(loopback_v4(), 0));
        const auto port = *server.local_port();
        bool timed_out = true;
        std::optional<std::expected<dns::detail::UdpAnswer, domain::DnsErrorInfo>> result;
        run_task([&]() -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn(answer_udp(server, kind));
                auto outcome = co_await coro::with_timeout(
                    1s, [&]() { return dns::detail::query_udp(loopback_v4(), port, query); });
                timed_out = outcome.timed_out;
                if (!timed_out)
                    result.emplace(std::move(*outcome));
            });
        }());
        ASSERT_FALSE(timed_out);
        ASSERT_TRUE(result);
        if (kind == UdpReplyCase::MALFORMED) {
            ASSERT_FALSE(*result);
            EXPECT_EQ(result->error().code, domain::DnsError::PARSE);
        } else {
            EXPECT_TRUE(result->has_value());
        }
    }
    auto invalid = query;
    invalid.resize(13);
    const auto local = run_task(dns::detail::query_udp(loopback_v4(), 0, invalid));
    ASSERT_FALSE(local);
    EXPECT_EQ(local.error().code, domain::DnsError::INVALID_QUERY);
}

coro::Task<void> flood_unrelated_replies(net::UdpSocket& server) {
    std::array<std::uint8_t, 4096> buffer{};
    const auto received = co_await server.recv_from(buffer);
    if (!received)
        co_return;
    std::vector<std::uint8_t> reply(buffer.begin(), buffer.begin() + received->size);
    reply[2] |= 0x80;
    reply[1] ^= 1;
    for (;;) {
        for (unsigned i = 0; i < 64; ++i) {
            if (!(co_await server.send_to(received->from, received->port, reply)))
                co_return;
        }
        co_await coro::sleep_for(1ms);
    }
}

TEST(NetCoroDns, strayFloodCannotResetTheUdpBudgetOrStarveItsTimer) {
    net::UdpSocket server;
    ASSERT_TRUE(server.bind(loopback_v4(), 0));
    const auto port = *server.local_port();
    const auto query = dns::QueryBuilder{}.add_question("x.test", dns::RecordType::A).build();
    bool timed_out = true;
    std::optional<std::expected<dns::detail::UdpAnswer, domain::DnsErrorInfo>> result;
    run_task([&]() -> coro::Task<void> {
        co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn(flood_unrelated_replies(server));
                auto outcome = co_await coro::with_timeout(
                    1300ms, [&]() { return dns::detail::query_udp(loopback_v4(), port, query); });
                timed_out = outcome.timed_out;
                if (!timed_out)
                    result.emplace(std::move(*outcome));
                scope.cancel();
            });
        });
    }());
    ASSERT_FALSE(timed_out);
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code, domain::DnsError::RETRY);
    EXPECT_NE(result->error().message.find("UDP query"), std::string::npos);
    // The reported budget is the constant, not a literal spelled into the message.
    EXPECT_NE(result->error().message.find(std::to_string(dns::Policy{}.wire_budget.count()) + "ms"),
              std::string::npos);
}

TEST(NetCoroDns, lateTruncationPreservesTcpFallbackTimeoutStage) {
    // A listening backlog completes TCP connect but never supplies a DNS reply.
    Utils::UniqueFd listener{::socket(AF_INET, SOCK_STREAM, 0)};
    ASSERT_GE(listener.get(), 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener.get(), 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length), 0);
    const auto port = ntohs(address.sin_port);
    net::UdpSocket server;
    ASSERT_TRUE(server.bind(loopback_v4(), port));
    const auto query = dns::QueryBuilder{}.add_question("x.test", dns::RecordType::A).build();
    std::optional<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> result;
    run_task([&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(answer_udp(server, UdpReplyCase::LATE_TC));
            result.emplace(co_await dns::detail::query_classic(loopback_v4(), port, query));
        });
    }());
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code, domain::DnsError::RETRY);
    EXPECT_NE(result->error().message.find("TCP fallback"), std::string::npos);
    EXPECT_NE(result->error().message.find(std::to_string(dns::Policy{}.attempt_budget.count()) + "ms"),
              std::string::npos);
}

// One deterministic bootstrap response; ignore the other family when requested.
coro::Task<void> answer_bootstrap(net::UdpSocket& server, const std::uint8_t rcode, const std::size_t address_length,
                                  const std::uint16_t wanted_type) {
    for (;;) {
        std::array<std::uint8_t, 4096> buffer{};
        const auto received = co_await server.recv_from(buffer);
        if (!received)
            co_return;
        const auto packet = std::span(buffer).first(received->size);
        const auto question = dns::RecordParser::parse_question(packet);
        if (question.qtype != wanted_type)
            continue;
        std::vector<std::uint8_t> response(packet.begin(), packet.end() - 11);  // Remove the empty query OPT.
        response[11] = 0;
        response[2] |= 0x80;
        response[3] = static_cast<std::uint8_t>(0x80 | rcode);
        if (address_length != 0) {
            response[7] = 1;
            response.insert(response.end(), {0xc0, 12, 0, static_cast<std::uint8_t>(wanted_type), 0, 1, 0, 0, 0, 60, 0,
                                             static_cast<std::uint8_t>(address_length)});
            response.resize(response.size() + address_length, 0);
            response.back() = 42;
        }
        const auto sent = co_await server.send_to(received->from, received->port, response);
        EXPECT_TRUE(sent.has_value());
        co_return;
    }
}

TEST(NetCoroDns, bootstrap_classifiesServerErrorsAndRejectsWrongAddressLength) {
    constexpr std::array<std::uint8_t, 3> RCODES{2, 5, 0};
    for (const std::uint8_t rcode : RCODES) {
        net::UdpSocket server;
        ASSERT_TRUE(server.bind(loopback_v4(), 0).has_value());
        const std::vector<domain::DnsServer> servers{{"127.0.0.1", *server.local_port()}};
        std::optional<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> result;
        run_task([&]() -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn(answer_bootstrap(server, rcode, rcode == 0 ? 16 : 0, 1));
                result.emplace(co_await dns::bootstrap_resolve("x.test", domain::AddressFamily::IPV4, servers));
            });
        }());
        ASSERT_TRUE(result.has_value());
        ASSERT_FALSE(result->has_value());
        EXPECT_EQ(result->error().code, rcode == 2   ? domain::DnsError::RETRY
                                        : rcode == 5 ? domain::DnsError::SERVER_REFUSED
                                                     : domain::DnsError::PARSE);
    }
}

TEST(NetCoroDns, bootstrap_returnsAvailableFamilyInsteadOfWaitingForSilentSibling) {
    for (const bool ipv6 : {false, true}) {
        net::UdpSocket server;
        ASSERT_TRUE(server.bind(loopback_v4(), 0).has_value());
        const std::vector<domain::DnsServer> servers{{"127.0.0.1", *server.local_port()}};
        bool timed_out = true;
        std::vector<domain::InetAddress> addresses;
        run_task([&]() -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn(answer_bootstrap(server, 0, ipv6 ? 16 : 4, ipv6 ? 28 : 1));
                auto outcome = co_await coro::with_timeout(
                    1s, [&]() { return dns::bootstrap_resolve("x.test", std::nullopt, servers); });
                timed_out = outcome.timed_out;
                if (outcome.has_value() && *outcome)
                    addresses = **outcome;
            });
        }());
        EXPECT_FALSE(timed_out);
        ASSERT_EQ(addresses.size(), 1U);
        EXPECT_EQ(addresses.front().get_family(), ipv6 ? domain::AddressFamily::IPV6 : domain::AddressFamily::IPV4);
    }
}

}  // namespace

TEST(NetCoroDns, Bootstrap_TwoSilentServers_StillReachesHealthyThirdWithinAttemptBudget) {
    net::UdpSocket silent_first;
    net::UdpSocket silent_second;
    net::UdpSocket healthy;
    ASSERT_TRUE(silent_first.bind(loopback_v4(), 0));
    ASSERT_TRUE(silent_second.bind(loopback_v4(), 0));
    ASSERT_TRUE(healthy.bind(loopback_v4(), 0));
    const std::vector<domain::DnsServer> servers{
        {"127.0.0.1", *silent_first.local_port()},
        {"127.0.0.1", *silent_second.local_port()},
        {"127.0.0.1", *healthy.local_port()},
    };
    std::optional<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> result;
    bool timed_out = true;
    run_task([&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(answer_bootstrap(healthy, 0, 4, 1));
            const auto outcome = co_await coro::with_timeout(dns::Policy{}.attempt_budget, [&]() {
                return dns::bootstrap_resolve("x.test", domain::AddressFamily::IPV4, servers);
            });
            timed_out = outcome.timed_out;
            if (outcome)
                result = *outcome;
            group.cancel();
        });
    }());
    EXPECT_FALSE(timed_out);
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result);
    ASSERT_EQ((*result)->size(), 1U);
    EXPECT_EQ((*result)->front().to_string(), "0.0.0.42");
}
