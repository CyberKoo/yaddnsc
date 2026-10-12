//
// Unit tests for the coroutine resolver layer: the DoT connect budget, the
// DoT/DoH catch-all mapping, the DoH per-address connect timeout and the
// bootstrap server policy.
//
// Everything runs on injected fakes (net::StreamFactory) over a ManualClock:
// no socket is opened and every deadline is virtual, so a black-holed endpoint
// costs no wall-clock time.
//
// ClassicResolver's catch-all is deliberately not covered here: query() builds
// its own UDP/TCP socket and exposes no injection seam, so an unexpected
// exception cannot be provoked without real I/O. The classic wire paths live in
// the component suite (test/component/net_coro_io_component_test.cpp).
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "coro/coro.h"
#include "domain/config/dns_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/dispatcher.h"
#include "infrastructure/dns/resolver/classic.h"
#include "infrastructure/dns/resolver/doh.h"
#include "infrastructure/dns/resolver/dot.h"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"

namespace {

using namespace std::chrono_literals;

using QueryResult = std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>;
using ResolveResult = std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>;

/// Run a task on a caller-owned loop (coro::run takes an already-built Task).
template<typename Fn>
void run_loop(coro::Loop& loop, Fn&& body) {
    coro::run(loop, body());
}

/// A stream whose connect never completes: the black-holed endpoint the DoT and
/// DoH connect budgets exist for. The park far outlives every budget under
/// test, so a budget that fails to fire shows up as a hung test, not a slow one.
class HangingStream final : public net::Stream {
public:
    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override {
        co_await coro::sleep_for(1h);
        co_return {};
    }

    [[nodiscard]] coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t>) override {
        co_return std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> read_exact(std::span<std::uint8_t>) override {
        co_return std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> send_all(std::span<const std::uint8_t>) override {
        co_return std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    void close() noexcept override {}

    [[nodiscard]] bool connected() const noexcept override { return false; }
};

/// Hands out hanging streams and records how often it was asked for one.
class HangingFactory final : public net::StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&, const net::TlsOptions&,
                                                          std::shared_ptr<const net::TlsContext>) override {
        ++tls_calls;
        return std::make_unique<HangingStream>();
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        ++tcp_calls;
        return std::make_unique<HangingStream>();
    }

    int tls_calls{0};
    int tcp_calls{0};
};

/// A defective factory: building a stream throws, as a resource or policy check
/// inside a real one would.
class ThrowingFactory final : public net::StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&, const net::TlsOptions&,
                                                          std::shared_ptr<const net::TlsContext>) override {
        throw std::runtime_error("stream factory unavailable");
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        throw std::runtime_error("stream factory unavailable");
    }
};

/// A stream that connects instantly but fails the first write: stands in for a
/// reachable-but-broken endpoint, so a test can observe that it was selected.
class FailSendStream final : public net::Stream {
public:
    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override { co_return {}; }

    [[nodiscard]] coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t>) override {
        co_return std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> read_exact(std::span<std::uint8_t>) override {
        co_return std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> send_all(std::span<const std::uint8_t>) override {
        co_return std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    void close() noexcept override {}

    [[nodiscard]] bool connected() const noexcept override { return true; }
};

/// Records the dialled addresses in order: the black-holed one hangs on
/// connect, every other connects but fails the write.
class FallbackFactory final : public net::StreamFactory {
public:
    explicit FallbackFactory(domain::InetAddress black_holed) : black_holed_(black_holed) {}

    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress address, std::uint16_t,
                                                          const net::ConnectOptions&, const net::TlsOptions&,
                                                          std::shared_ptr<const net::TlsContext>) override {
        dialled.push_back(address);
        if (address == black_holed_) {
            return std::make_unique<HangingStream>();
        }
        return std::make_unique<FailSendStream>();
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        return std::make_unique<HangingStream>();
    }

    std::vector<domain::InetAddress> dialled;

private:
    domain::InetAddress black_holed_;
};

/// A DoH endpoint on an IP literal: the URL host bypasses the injected resolver.
constexpr const char* DOH_URL = "https://203.0.113.10/dns-query";

// ---------------------------------------------------------------------------
// DoT: the resolve and each address's connect have their own budget
// ---------------------------------------------------------------------------

TEST(DotResolver, connectBudgetExpiresAsRetryWhenTheStreamNeverConnects) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto factory = std::make_shared<HangingFactory>();
    dns::EndpointOptions options;
    options.factory = factory;
    // An IP-literal endpoint skips bootstrap resolution, so the budget can only
    // be spent on the connect itself.
    dns::DotResolver resolver{"203.0.113.10", 853, options};
    const auto start = clock.now();

    std::optional<QueryResult> result;
    bool outer_budget_fired = false;
    run_loop(loop, [&]() -> coro::Task<void> {
        // The outer budget bounds the test only: a resolver that never gave up
        // would be cancelled here instead of failing on its own budget.
        auto outcome = co_await coro::with_timeout(30s, [&]() -> coro::Task<QueryResult> {
            co_return co_await resolver.query("x.test", domain::RecordKind::A);
        });
        outer_budget_fired = outcome.timed_out;
        if (outcome.has_value()) {
            result.emplace(std::move(*outcome));
        }
        co_return;
    });

    EXPECT_FALSE(outer_budget_fired);
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, domain::DnsError::RETRY);
    // One attempt; whole-query retry belongs to the dispatcher.
    EXPECT_EQ(factory->tls_calls, 1);
    EXPECT_EQ(clock.now() - start, 500ms);
}

TEST(DotResolver, multiAddressFallbackGivesEachAddressItsOwnConnectBudget) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto black_holed = *domain::InetAddress::parse("203.0.113.10");
    const auto reachable = *domain::InetAddress::parse("203.0.113.11");
    auto factory = std::make_shared<FallbackFactory>(black_holed);
    dns::EndpointOptions options;
    options.factory = factory;
    options.resolve = [black_holed, reachable](std::string,
                                               std::optional<domain::AddressFamily>) -> coro::Task<ResolveResult> {
        co_return std::vector<domain::InetAddress>{black_holed, reachable};
    };
    // A named endpoint exercises the resolve path; the injected lookup above
    // keeps it off the network.
    dns::DotResolver resolver{"dot.test", 853, options};
    const auto start = clock.now();

    std::optional<QueryResult> result;
    run_loop(loop, [&]() -> coro::Task<void> {
        result.emplace(co_await resolver.query("x.test", domain::RecordKind::A));
        co_return;
    });

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    // The second address connected and failed the write.
    EXPECT_EQ(result->error().code, domain::DnsError::CONNECTION);
    // A black-holed first address spends only its own budget: the second
    // address is dialled on every attempt instead of being cancelled together
    // with the first address's expired budget.
    const std::vector<domain::InetAddress> expected{black_holed, reachable};
    EXPECT_EQ(factory->dialled, expected);
    EXPECT_EQ(clock.now() - start, 500ms);
}

TEST(DotResolver, nameResolutionConsumesTheWholeAttemptBudget) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto factory = std::make_shared<HangingFactory>();
    dns::EndpointOptions options;
    options.factory = factory;
    options.resolve = [](std::string, std::optional<domain::AddressFamily>) -> coro::Task<ResolveResult> {
        co_await coro::sleep_for(1h);  // outlives the budget; cancellation unwinds it
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::RETRY, "unreachable"});
    };
    dns::DotResolver resolver{"dot.test", 853, options};
    const auto start = clock.now();

    std::optional<QueryResult> result;
    run_loop(loop, [&]() -> coro::Task<void> {
        result.emplace(co_await resolver.query("x.test", domain::RecordKind::A));
        co_return;
    });

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, domain::DnsError::CONNECTION);
    EXPECT_NE(result->error().message.find("attempt timed out"), std::string::npos);
    EXPECT_EQ(factory->tls_calls, 0);
    EXPECT_EQ(clock.now() - start, 1500ms);
}

TEST(DotResolver, Query_FactoryDefect_PropagatesToOperationBoundary) {
    auto factory = std::make_shared<ThrowingFactory>();
    dns::EndpointOptions options;
    options.factory = factory;
    dns::DotResolver resolver{"203.0.113.10", 853, options};

    EXPECT_THROW(
        { [[maybe_unused]] auto result = coro::run(resolver.query("x.test", domain::RecordKind::A)); },
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// DoH: the catch-all and the per-address connect timeout
// ---------------------------------------------------------------------------

TEST(DohResolver, Query_FactoryDefect_PropagatesToOperationBoundary) {
    auto factory = std::make_shared<ThrowingFactory>();
    http::Options options;
    options.factory = factory;
    dns::DohResolver resolver{DOH_URL, options};

    EXPECT_THROW(
        { [[maybe_unused]] auto result = coro::run(resolver.query("x.test", domain::RecordKind::A)); },
        std::runtime_error);
}

TEST(DohResolver, connectTimeoutFailsAsConnectionBeforeTheExchangeBudget) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto factory = std::make_shared<HangingFactory>();
    http::Options options;
    options.factory = factory;
    options.connect_timeout = 100ms;
    dns::DohResolver resolver{DOH_URL, options};
    const auto start = clock.now();

    std::optional<QueryResult> result;
    run_loop(loop, [&]() -> coro::Task<void> {
        result.emplace(co_await resolver.query("x.test", domain::RecordKind::A));
        co_return;
    });

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    // A black-holed endpoint is a connection failure, not a stalled exchange.
    EXPECT_EQ(result->error().code, domain::DnsError::CONNECTION);
    EXPECT_EQ(factory->tls_calls, 1);
    EXPECT_EQ(clock.now() - start, 100ms);
}

// ---------------------------------------------------------------------------
// DoT: a bootstrap verdict about the endpoint is not a verdict on the query
// ---------------------------------------------------------------------------

/// Injected endpoint resolution that always fails with `code`.
[[nodiscard]] net::HostResolver failing_resolve(const domain::DnsError code, std::string message) {
    return [code, message = std::move(message)](std::string,
                                                std::optional<domain::AddressFamily>) -> coro::Task<ResolveResult> {
        co_return std::unexpected(domain::DnsErrorInfo{code, message});
    };
}

TEST(DotResolver, endpointNxdomainFallsBackAsConnection) {
    // NXDOMAIN for the endpoint host says nothing about the queried name, so
    // it must surface as a retryable connection failure instead of the
    // dispatcher's negative answer about the caller's name.
    dns::EndpointOptions options;
    options.factory = std::make_shared<ThrowingFactory>();  // never reached: resolution fails first
    options.resolve = failing_resolve(domain::DnsError::NX_DOMAIN, "injected NXDOMAIN");
    dns::DotResolver resolver{"dot.test", 853, std::move(options)};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONNECTION);
    EXPECT_TRUE(dns::detail::is_retryable(result.error().code));
    EXPECT_NE(result.error().message.find(R"(DoT endpoint "dot.test" resolution failed: injected NXDOMAIN)"),
              std::string::npos);
}

TEST(DotResolver, endpointNodataFallsBackAsConnection) {
    dns::EndpointOptions options;
    options.factory = std::make_shared<ThrowingFactory>();  // never reached: resolution fails first
    options.resolve = failing_resolve(domain::DnsError::NODATA, "injected NODATA");
    dns::DotResolver resolver{"dot.test", 853, std::move(options)};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONNECTION);
    EXPECT_TRUE(dns::detail::is_retryable(result.error().code));
}

TEST(DotResolver, endpointConfigFailureIsScopedToTheEndpoint) {
    dns::EndpointOptions options;
    options.factory = std::make_shared<ThrowingFactory>();  // never reached: resolution fails first
    options.resolve = failing_resolve(domain::DnsError::CONFIG, "no bootstrap DNS servers are configured");
    dns::DotResolver resolver{"dot.test", 853, std::move(options)};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONNECTION);
    EXPECT_FALSE(dns::detail::is_definitive(result.error().code));
    EXPECT_NE(result.error().message.find("no bootstrap DNS servers are configured"), std::string::npos);
}

// ---------------------------------------------------------------------------
// DoH: the HTTP-to-DNS failure mapping direction
// ---------------------------------------------------------------------------

/// A stream that connects instantly and replays a scripted response once.
class ScriptedStream final : public net::Stream {
public:
    explicit ScriptedStream(std::string script) : script_(std::move(script)) {}

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override { co_return {}; }

    [[nodiscard]] coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t> buf) override {
        const std::size_t count = std::min(buf.size(), script_.size() - cursor_);
        std::memcpy(buf.data(), script_.data() + cursor_, count);
        cursor_ += count;
        co_return count;
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> read_exact(std::span<std::uint8_t> buf) override {
        std::size_t filled = 0;
        while (filled < buf.size()) {
            auto read = co_await read_some(buf.subspan(filled));
            if (!read || *read == 0) {
                co_return std::unexpected(net::IoError::CONNECTION_FAILED);
            }
            filled += *read;
        }
        co_return {};
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> send_all(std::span<const std::uint8_t>) override {
        co_return {};
    }

    void close() noexcept override {}

    [[nodiscard]] bool connected() const noexcept override { return true; }

private:
    std::string script_;
    std::size_t cursor_{0};
};

/// Hands out a fresh ScriptedStream per dial, replaying the same script, so
/// each dispatcher attempt can read the script again.
class ScriptFactory final : public net::StreamFactory {
public:
    explicit ScriptFactory(std::string script) : script_(std::move(script)) {}

    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&, const net::TlsOptions&,
                                                          std::shared_ptr<const net::TlsContext>) override {
        return std::make_unique<ScriptedStream>(script_);
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        return std::make_unique<ScriptedStream>(script_);
    }

private:
    std::string script_;
};

TEST(DohResolver, oversizedResponseAllowsFailoverWithoutSameBackendRetry) {
    // PARSE describes this endpoint's unusable reply; it does not end the group search.
    http::Options options;
    options.limits.max_header_bytes = 64;
    options.factory = std::make_shared<ScriptFactory>(
        "HTTP/1.1 200 OK\r\nContent-Type: application/dns-message\r\nContent-Length: 0\r\n\r\n");
    dns::DohResolver resolver{DOH_URL, options};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_FALSE(dns::detail::is_definitive(result.error().code));
}

// ---------------------------------------------------------------------------
// Oversized responses fail over, on DoH's body cap and DoT's length prefix
// ---------------------------------------------------------------------------

TEST(DohResolver, oversizedBodyAllowsFailoverWithoutSameBackendRetry) {
    // The Content-Length check fires at header parse, so the script ends with
    // the headers: the body is never read.
    http::Options options;
    options.limits.max_body_bytes = 64;
    options.factory = std::make_shared<ScriptFactory>(
        "HTTP/1.1 200 OK\r\nContent-Type: application/dns-message\r\nContent-Length: 100\r\n\r\n");
    dns::DohResolver resolver{DOH_URL, options};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_FALSE(dns::detail::is_definitive(result.error().code));
}

TEST(DotResolver, oversizedAnnouncedLengthAllowsFailover) {
    // 0xFFFF announces 65535 octets — past MAX_RESPONSE_SIZE, so the resolver
    // rejects the endpoint right after the prefix without buffering the body.
    dns::EndpointOptions options;
    options.factory = std::make_shared<ScriptFactory>("\xff\xff");
    dns::DotResolver resolver{"203.0.113.10", 853, options};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_FALSE(dns::detail::is_definitive(result.error().code));
}

TEST(DohResolver, protocolUpgradeAnswerAllowsFailover) {
    http::Options options;
    options.factory = std::make_shared<ScriptFactory>(
        "HTTP/1.1 101 Switching Protocols\r\nConnection: upgrade\r\nUpgrade: h2c\r\nContent-Length: 0\r\n\r\n");
    dns::DohResolver resolver{DOH_URL, options};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_FALSE(dns::detail::is_definitive(result.error().code));
}

TEST(DohResolver, unusableRemoteResponsesAllowFailover) {
    const std::vector<std::string> scripts{
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: invalid\r\n\r\n",
        // Correct Content-Type, but a body too short to contain a DNS header.
        "HTTP/1.1 200 OK\r\nContent-Type: application/dns-message\r\nContent-Length: 1\r\n\r\nx",
    };
    for (const auto& script : scripts) {
        SCOPED_TRACE(script);
        http::Options options;
        options.factory = std::make_shared<ScriptFactory>(script);
        dns::DohResolver resolver{DOH_URL, options};

        const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
        EXPECT_FALSE(dns::detail::is_definitive(result.error().code));
    }
}

// ---------------------------------------------------------------------------
// packet construction failure is INVALID_QUERY in every resolver
// ---------------------------------------------------------------------------

/// A hostname whose first label exceeds 63 octets: building the query throws
/// DnsPacketException before any I/O, in every resolver.
[[nodiscard]] const std::string& overlong_label_host() {
    static const std::string host = std::string(64, 'x') + ".test";
    return host;
}

TEST(DotResolver, packetConstructionFailureIsInvalidQuery) {
    dns::EndpointOptions options;
    options.factory = std::make_shared<ThrowingFactory>();
    dns::DotResolver resolver{"203.0.113.10", 853, options};
    const auto result = coro::run(resolver.query(overlong_label_host(), domain::RecordKind::A));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::INVALID_QUERY);
    EXPECT_TRUE(dns::detail::is_definitive(result.error().code));
}

TEST(DohResolver, packetConstructionFailureIsInvalidQuery) {
    http::Options options;
    options.factory = std::make_shared<ThrowingFactory>();
    dns::DohResolver resolver{DOH_URL, options};
    const auto result = coro::run(resolver.query(overlong_label_host(), domain::RecordKind::A));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::INVALID_QUERY);
    EXPECT_TRUE(dns::detail::is_definitive(result.error().code));
}

TEST(ClassicResolver, packetConstructionFailureIsInvalidQuery) {
    dns::ClassicResolver resolver{*domain::InetAddress::parse("203.0.113.10"), 53};
    const auto result = coro::run(resolver.query(overlong_label_host(), domain::RecordKind::A));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::INVALID_QUERY);
    EXPECT_TRUE(dns::detail::is_definitive(result.error().code));
}

// ---------------------------------------------------------------------------
// bootstrap server policy
// ---------------------------------------------------------------------------

TEST(BootstrapResolve, nonLiteralServerIsSkippedAndReportedAsConfig) {
    // No network: the only configured server is rejected before any query.
    const auto result = coro::run(dns::bootstrap_resolve("example.org", std::nullopt, {{"not-an-ip", 53}}));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result.error().message.find("IP literal"), std::string::npos);
}

TEST(BootstrapResolve, zeroPortServerIsSkippedAndReportedAsConfig) {
    // The other half of the server-entry policy: the address parses, so only the
    // port is wrong. It must name that defect and not the address one, and no
    // query is ever sent towards the missing port.
    const auto result = coro::run(dns::bootstrap_resolve("example.org", std::nullopt, {{"192.0.2.1", 0}}));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result.error().message.find("nonzero port"), std::string::npos);
    EXPECT_EQ(result.error().message.find("IP literal"), std::string::npos);
}

TEST(BootstrapResolve, packetConstructionFailureIsInvalidQuery) {
    const auto result = coro::run(dns::bootstrap_resolve(overlong_label_host(), std::nullopt, {{"192.0.2.1", 53}}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::INVALID_QUERY);
    EXPECT_TRUE(dns::detail::is_definitive(result.error().code));
}

TEST(BootstrapResolve, definitiveErrorSurvivesALaterTransientFailure) {
    // One real UDP socket towards TEST-NET-1 (192.0.2.1): the send is queued
    // silently and the receive is cut by the UDP budget on the manual clock,
    // so the transient failure costs no wall-clock time. The first server's
    // CONFIG diagnosis must survive the second server's transient failure.
    coro::ManualClock clock;
    coro::Loop loop{clock};
    std::optional<ResolveResult> result;
    // A named list: a braced-init-list argument inside the coroutine lambda
    // trips a GCC modules-ts gimplifier ICE.
    const std::vector<domain::DnsServer> servers{{"not-an-ip", 53}, {"192.0.2.1", 53}};
    run_loop(loop, [&]() -> coro::Task<void> {
        result.emplace(co_await dns::bootstrap_resolve("example.org", domain::AddressFamily::IPV4, servers));
        co_return;
    });

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result->error().message.find("IP literal"), std::string::npos);
}

TEST(BootstrapResolve, nonLiteralServerAddressIsRedactedInTheError) {
    // A configured address may embed URI userinfo; the error message must not
    // leak it into the logs it is destined for.
    const auto result =
        coro::run(dns::bootstrap_resolve("example.org", std::nullopt, {{"https://user:secret@not-an-ip", 53}}));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_EQ(result.error().message.find("secret"), std::string::npos);
    EXPECT_NE(result.error().message.find("***"), std::string::npos);
}

TEST(DohResolver, blackHoledFirstAddressDoesNotPreventTryingTheSecond) {
    const auto dead = *domain::InetAddress::parse("203.0.113.10");
    const auto good = *domain::InetAddress::parse("203.0.113.11");
    auto factory = std::make_shared<FallbackFactory>(dead);
    http::Options options;
    options.factory = factory;
    options.connect_timeout = 1s;  // Constructor caps this below the complete attempt.
    options.resolve = [dead, good](std::string, std::optional<domain::AddressFamily>) -> coro::Task<ResolveResult> {
        co_return std::vector<domain::InetAddress>{dead, good};
    };
    dns::DohResolver resolver{"https://doh.test/dns-query", options};
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto result = coro::run(loop, resolver.query("x.test", domain::RecordKind::A));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(factory->dialled, (std::vector<domain::InetAddress>{dead, good}));
}

TEST(DohResolver, directConstructionEnforcesHttpsAndTheResponseCap) {
    EXPECT_THROW(
        { dns::DohResolver resolver("http://203.0.113.10/dns-query", http::Options{}); }, std::invalid_argument);
    http::Options options;
    options.factory = std::make_shared<ScriptFactory>(
        "HTTP/1.1 200 OK\r\nContent-Type: application/dns-message\r\nContent-Length: 20000\r\n\r\n");
    dns::DohResolver resolver{DOH_URL, options};
    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_NE(result.error().message.find("Content-Length exceeds limit"), std::string::npos);
}

TEST(DohResolver, rateLimitingIsRetryable) {
    http::Options options;
    options.factory = std::make_shared<ScriptFactory>("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n");
    dns::DohResolver resolver{DOH_URL, options};
    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::RETRY);
}

class StalledReadStream final : public net::Stream {
public:
    explicit StalledReadStream(int& closed) : closed_(closed) {}

    coro::Task<std::expected<void, net::IoError>> ensure_connected() override { co_return {}; }

    coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t>) override {
        co_await coro::sleep_for(1h);
        co_return 0;
    }

    coro::Task<std::expected<void, net::IoError>> read_exact(std::span<std::uint8_t>) override {
        co_await coro::sleep_for(1h);
        co_return {};
    }

    coro::Task<std::expected<void, net::IoError>> send_all(std::span<const std::uint8_t>) override { co_return {}; }

    void close() noexcept override {
        if (connected_)
            ++closed_;
        connected_ = false;
    }

    bool connected() const noexcept override { return connected_; }

private:
    int& closed_;
    bool connected_{true};
};

class StalledReadFactory final : public net::StreamFactory {
public:
    std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t, const net::ConnectOptions&,
                                            const net::TlsOptions&, std::shared_ptr<const net::TlsContext>) override {
        ++tls_calls;
        return std::make_unique<StalledReadStream>(closed);
    }

    std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t, const net::ConnectOptions&) override {
        return std::make_unique<StalledReadStream>(closed);
    }

    int tls_calls{0};
    int closed{0};
};

// Echo one x.test query, then simulate an idle peer that closed the connection.
class StaleEchoStream final : public net::Stream {
public:
    explicit StaleEchoStream(bool dot) : dot_(dot) {}

    coro::Task<std::expected<void, net::IoError>> ensure_connected() override { co_return {}; }

    coro::Task<std::expected<void, net::IoError>> send_all(std::span<const std::uint8_t> bytes) override {
        if (sent_)
            co_return std::unexpected(net::IoError::CONNECTION_FAILED);
        sent_ = true;
        std::span<const std::uint8_t> query;
        if (dot_) {
            query = bytes.subspan(2);
        } else {
            const std::string_view wire(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            query = bytes.subspan(wire.find("\r\n\r\n") + 4);
        }
        const auto id = static_cast<std::uint16_t>((static_cast<std::uint16_t>(query[0]) << 8) | query[1]);
        auto answer = dns::QueryBuilder{}.id(id).qr(true).ra(true).add_question("x.test", dns::RecordType::A).build();
        answer[7] = 1;
        answer.insert(answer.end(), {0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 0, 2, 42});
        if (dot_) {
            response_.push_back(static_cast<std::uint8_t>(answer.size() >> 8));
            response_.push_back(static_cast<std::uint8_t>(answer.size()));
        } else {
            response_ = "HTTP/1.1 200 OK\r\nContent-Type: application/dns-message\r\nContent-Length: " +
                        std::to_string(answer.size()) + "\r\n\r\n";
        }
        response_.append(reinterpret_cast<const char*>(answer.data()), answer.size());
        co_return {};
    }

    coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t> out) override {
        const auto count = std::min(out.size(), response_.size() - cursor_);
        std::memcpy(out.data(), response_.data() + cursor_, count);
        cursor_ += count;
        co_return count;
    }

    coro::Task<std::expected<void, net::IoError>> read_exact(std::span<std::uint8_t> out) override {
        const auto count = co_await read_some(out);
        if (!count || *count != out.size())
            co_return std::unexpected(net::IoError::CONNECTION_FAILED);
        co_return {};
    }

    void close() noexcept override { connected_ = false; }

    bool connected() const noexcept override { return connected_; }

private:
    bool dot_;
    bool sent_{false};
    bool connected_{true};
    std::string response_;
    std::size_t cursor_{0};
};

class StaleEchoFactory final : public net::StreamFactory {
public:
    explicit StaleEchoFactory(bool dot) : dot_(dot) {}

    std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t, const net::ConnectOptions&,
                                            const net::TlsOptions&, std::shared_ptr<const net::TlsContext>) override {
        ++tls_calls;
        return std::make_unique<StaleEchoStream>(dot_);
    }

    std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t, const net::ConnectOptions&) override {
        return std::make_unique<StaleEchoStream>(dot_);
    }

    int tls_calls{0};

private:
    bool dot_;
};

TEST(EncryptedResolvers, lostReusedConnectionIsRepairedWithinTheSameAttempt) {
    for (const bool dot : {false, true}) {
        auto factory = std::make_shared<StaleEchoFactory>(dot);
        std::unique_ptr<dns::Resolver> resolver;
        if (dot) {
            dns::EndpointOptions options;
            options.factory = factory;
            resolver = std::make_unique<dns::DotResolver>("203.0.113.10", 853, options);
        } else {
            http::Options options;
            options.factory = factory;
            resolver = std::make_unique<dns::DohResolver>(DOH_URL, options);
        }
        coro::run([&]() -> coro::Task<void> {
            for (int i = 0; i < 2; ++i) {
                const auto result = co_await resolver->query("x.test", domain::RecordKind::A);
                EXPECT_TRUE(result.has_value());
            }
        }());
        EXPECT_EQ(factory->tls_calls, 2);
    }
}

TEST(DotResolver, closeWhileAnActiveQueryOwnsTheStreamIsDeferred) {
    auto factory = std::make_shared<StalledReadFactory>();
    dns::EndpointOptions options;
    options.factory = factory;
    dns::DotResolver resolver{"203.0.113.10", 853, options};
    coro::ManualClock clock;
    coro::Loop loop{clock};
    auto owner = [&]() -> coro::Task<void> {
        const auto result = co_await resolver.query("x.test", domain::RecordKind::A);
        EXPECT_FALSE(result.has_value());
    };
    run_loop(loop, [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(owner());
            co_await coro::sleep_for(5ms);
            resolver.close();
            EXPECT_EQ(factory->closed, 0);
        });
        const auto next = co_await resolver.query("x.test", domain::RecordKind::A);
        EXPECT_FALSE(next.has_value());
    });
    EXPECT_EQ(factory->tls_calls, 2);
    EXPECT_EQ(factory->closed, 2);
}

TEST(EncryptedResolvers, wholeQueryRetriesFitInsideTheApplicationBudget) {
    for (const bool dot : {false, true}) {
        for (const int count : {1, 2}) {
            auto factory = std::make_shared<StalledReadFactory>();
            std::vector<std::unique_ptr<dns::Resolver>> backends;
            for (int i = 0; i < count; ++i) {
                if (dot) {
                    dns::EndpointOptions options;
                    options.factory = factory;
                    backends.push_back(std::make_unique<dns::DotResolver>("203.0.113.10", 853, options));
                } else {
                    http::Options options;
                    options.factory = factory;
                    backends.push_back(std::make_unique<dns::DohResolver>(DOH_URL, options));
                }
            }
            dns::Dispatcher dispatcher{std::move(backends)};
            coro::ManualClock clock;
            coro::Loop loop{clock};
            const auto start = clock.now();
            bool timed_out = true;
            std::optional<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> result;
            run_loop(loop, [&]() -> coro::Task<void> {
                auto outcome = co_await coro::with_timeout(
                    4s, [&]() { return dispatcher.resolve("x.test", domain::RecordKind::A); });
                timed_out = outcome.timed_out;
                if (outcome.has_value())
                    result.emplace(std::move(*outcome));
            });
            EXPECT_FALSE(timed_out);
            ASSERT_TRUE(result.has_value());
            ASSERT_FALSE(result->has_value());
            EXPECT_EQ(factory->tls_calls, 2 * count);
            EXPECT_EQ(factory->closed, 2 * count);
            EXPECT_EQ(clock.now() - start, 3050ms);
        }
    }
}

TEST(EncryptedResolvers, Query_CancelledSessionWait_LeavesActiveConnectionUntouched) {
    for (const bool dot : {false, true}) {
        auto factory = std::make_shared<StalledReadFactory>();
        std::unique_ptr<dns::Resolver> resolver;
        if (dot) {
            dns::EndpointOptions options;
            options.factory = factory;
            resolver = std::make_unique<dns::DotResolver>("203.0.113.10", 853, options);
        } else {
            http::Options options;
            options.factory = factory;
            resolver = std::make_unique<dns::DohResolver>(DOH_URL, options);
        }
        coro::ManualClock clock;
        coro::Loop loop{clock};
        auto owner = [&]() -> coro::Task<void> {
            const auto result = co_await resolver->query("x.test", domain::RecordKind::A);
            EXPECT_FALSE(result.has_value());
        };
        run_loop(loop, [&]() -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                group.spawn(owner());
                co_await coro::sleep_for(5ms);
                const auto wait = co_await coro::with_timeout(
                    500ms, [&]() { return resolver->query("x.test", domain::RecordKind::A); });
                EXPECT_TRUE(wait.timed_out);
                EXPECT_EQ(factory->closed, 0);
            });
        });
        EXPECT_EQ(factory->tls_calls, 1);
        EXPECT_EQ(factory->closed, 1);
    }
}

TEST(BootstrapResolve, literalAddressMustMatchRequestedFamily) {
    // A literal of the wrong family is a local parameter mismatch: CONFIG, not a
    // NODATA claim about a name that exists without the requested records.
    const auto result = coro::run(dns::bootstrap_resolve("::1", domain::AddressFamily::IPV4, {}));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result.error().message.find("::1"), std::string::npos);
    EXPECT_NE(result.error().message.find("does not match the requested address family"), std::string::npos);
}

class DelayedEchoStream final : public net::Stream {
public:
    explicit DelayedEchoStream(bool dot) : inner_(dot) {}

    coro::Task<std::expected<void, net::IoError>> ensure_connected() override {
        co_return co_await inner_.ensure_connected();
    }

    coro::Task<std::expected<void, net::IoError>> send_all(std::span<const std::uint8_t> bytes) override {
        auto result = co_await inner_.send_all(bytes);
        if (result)
            co_await coro::sleep_for(300ms);
        co_return result;
    }

    coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t> bytes) override {
        co_return co_await inner_.read_some(bytes);
    }

    coro::Task<std::expected<void, net::IoError>> read_exact(std::span<std::uint8_t> bytes) override {
        co_return co_await inner_.read_exact(bytes);
    }

    void close() noexcept override { inner_.close(); }

    bool connected() const noexcept override { return inner_.connected(); }

private:
    StaleEchoStream inner_;
};

class DelayedEchoFactory final : public net::StreamFactory {
public:
    explicit DelayedEchoFactory(bool dot) : dot_(dot) {}

    std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t, const net::ConnectOptions&,
                                            const net::TlsOptions&, std::shared_ptr<const net::TlsContext>) override {
        return std::make_unique<DelayedEchoStream>(dot_);
    }

    std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t, const net::ConnectOptions&) override {
        return std::make_unique<DelayedEchoStream>(dot_);
    }

private:
    bool dot_;
};

TEST(EncryptedResolvers, Query_ConcurrentHealthyRequests_WaitWithoutBackendRetry) {
    for (bool dot : {true, false}) {
        auto factory = std::make_shared<DelayedEchoFactory>(dot);
        std::unique_ptr<dns::Resolver> resolver;
        if (dot) {
            dns::EndpointOptions options;
            options.factory = factory;
            resolver = std::make_unique<dns::DotResolver>("203.0.113.10", 853, options);
        } else {
            http::Options options;
            options.factory = factory;
            resolver = std::make_unique<dns::DohResolver>(DOH_URL, options);
        }
        coro::ManualClock clock;
        coro::Loop loop{clock};
        int successes = 0, queue_failures = 0;
        auto query = [&]() -> coro::Task<void> {
            auto result = co_await resolver->query("x.test", domain::RecordKind::A);
            if (result)
                ++successes;
            else if (result.error().message.find("queue") != std::string::npos)
                ++queue_failures;
        };
        run_loop(loop, [&]() -> coro::Task<void> {
            co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
                for (int i = 0; i < 4; ++i)
                    group.spawn(query());
                co_return;
            });
        });
        EXPECT_EQ(successes, 4);
        EXPECT_EQ(queue_failures, 0);
    }
}

}  // namespace

TEST(DotResolver, Query_InjectedPolicy_UsesConfiguredConnectBudget) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    dns::EndpointOptions options;
    options.factory = std::make_shared<HangingFactory>();
    options.policy.connect_budget = 25ms;
    options.policy.attempt_budget = 100ms;
    dns::DotResolver resolver{"203.0.113.10", 853, options};
    const auto start = clock.now();
    const auto result = coro::run(loop, resolver.query("x.test", domain::RecordKind::A));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::RETRY);
    EXPECT_EQ(clock.now() - start, 25ms);
}
