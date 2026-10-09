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

#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/coro/coro.h"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/resolver/classic.h"
#include "infrastructure/dns/dispatcher.h"
#include "infrastructure/dns/resolver/doh.h"
#include "infrastructure/dns/resolver/dot.h"
#include "infrastructure/http/client.h"
#include "infrastructure/http/persistent_client.h"
#include "infrastructure/network/tls/context.h"
#include "support/fmt.hpp"
#include "support/util/fd.hpp"

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

/// Build the off-loop trust context a resolver's TLS options ask for.
[[nodiscard]] std::shared_ptr<const net::TlsContext> client_context(const net::TlsOptions& options) {
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
        ::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));

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
                ::connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address));
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

    /// Probe a TCP listener until it accepts or the budget runs out.
    [[nodiscard]] bool wait_for_tcp(const std::uint16_t port, const std::chrono::milliseconds budget = 10s) {
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
            std::this_thread::sleep_for(100ms);
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
    const std::string cert = directory + "/cert.pem";
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
    if (!server.wait_for_tcp(CLASSIC_PORT)) {
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
    if (!server.wait_for_tcp(DOH_PORT)) {
        GTEST_SKIP() << "doh_server.py did not start";
    }

    http::Options options;
    options.tls.verify_peer = false;
    options.tls_context = client_context(options.tls);
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
    if (!server.wait_for_tcp(DOT_PORT)) {
        GTEST_SKIP() << "dot_server.py did not start";
    }

    dns::EndpointOptions options;
    options.tls.verify_peer = false;
    options.tls_context = client_context(options.tls);
    dns::DotResolver resolver{"127.0.0.1", DOT_PORT, options};

    auto resolve = [&]() -> coro::Task<std::pair<bool, bool>> {
        auto first = co_await resolver.query("yaddnsc.test", domain::RecordKind::A);
        auto second = co_await resolver.query("yaddnsc.test", domain::RecordKind::A);
        co_return std::pair{first.has_value() && !first->empty(), second.has_value() && !second->empty()};
    };

    const auto [first_ok, second_ok] = run_task(resolve());
    EXPECT_TRUE(first_ok);   // padded query answered
    EXPECT_TRUE(second_ok);  // second query on the persistent connection
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
    if (!server.wait_for_tcp(DOT_TIMEOUT_PORT)) {
        GTEST_SKIP() << "dot_server.py did not start";
    }

    dns::EndpointOptions options;
    options.tls.verify_peer = false;
    options.tls_context = client_context(options.tls);
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
    if (!server.wait_for_tcp(DOH_PORT)) {
        GTEST_SKIP() << "doh_server.py did not start";
    }

    http::Options options;
    options.tls.verify_peer = false;
    options.tls_context = client_context(options.tls);
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
    if (!server.wait_for_tcp(CLASSIC_PORT)) {
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
    if (!first.wait_for_tcp(BOOTSTRAP_FIRST_PORT) || !second.wait_for_tcp(BOOTSTRAP_SECOND_PORT)) {
        GTEST_SKIP() << "dns_server.py did not start";
    }

    const std::vector<domain::DnsServer> servers{{"127.0.0.1", static_cast<std::uint16_t>(BOOTSTRAP_FIRST_PORT)},
                                                 {"127.0.0.1", static_cast<std::uint16_t>(BOOTSTRAP_SECOND_PORT)}};
    auto resolve = [&]() -> coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> {
        co_return co_await dns::bootstrap_resolve("nodata.yaddnsc.test", domain::AddressFamily::IPV4, servers);
    };

    const auto result = run_task(resolve());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(result->front().to_string(), "198.51.100.77");
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
    if (!first.wait_for_tcp(BOOTSTRAP_FIRST_PORT) || !second.wait_for_tcp(BOOTSTRAP_SECOND_PORT)) {
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

}  // namespace
