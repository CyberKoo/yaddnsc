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

#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include "infrastructure/dns/resolver/doh.h"
#include "infrastructure/dns/resolver/dot.h"
#include "infrastructure/dns/resolver/resolver.h"
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
    // Both attempts dialled the endpoint and each one spent the full connect
    // budget, so the query failed after 2 × CONNECT_BUDGET rather than parking
    // until the caller's scope fired.
    EXPECT_EQ(factory->tls_calls, 2);
    EXPECT_EQ(clock.now() - start, 2s);
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
    // The second address connected and failed the write, on both attempts.
    EXPECT_EQ(result->error().code, domain::DnsError::CONNECTION);
    // A black-holed first address spends only its own budget: the second
    // address is dialled on every attempt instead of being cancelled together
    // with the first address's expired budget.
    const std::vector<domain::InetAddress> expected{black_holed, reachable, black_holed, reachable};
    EXPECT_EQ(factory->dialled, expected);
    // 2 attempts × 1 black-holed connect budget; the rest is instant.
    EXPECT_EQ(clock.now() - start, 2s);
}

TEST(DotResolver, nameResolutionBudgetExpiresAsRetry) {
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
    EXPECT_EQ(result->error().code, domain::DnsError::RETRY);
    EXPECT_NE(result->error().message.find("name resolution"), std::string::npos);
    // The resolve timed out before any address existed, so nothing was dialled.
    EXPECT_EQ(factory->tls_calls, 0);
    // Both attempts spent the resolution budget: 2 × CONNECT_BUDGET.
    EXPECT_EQ(clock.now() - start, 2s);
}

TEST(DotResolver, factoryDefectIsUnknownSoTheSearchFallsOver) {
    auto factory = std::make_shared<ThrowingFactory>();
    dns::EndpointOptions options;
    options.factory = factory;
    dns::DotResolver resolver{"203.0.113.10", 853, options};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::UNKNOWN);
    // An unexpected exception is not a deterministic parse failure: it must
    // stay retryable so the dispatcher tries the next backend.
    EXPECT_TRUE(dns::detail::is_retryable(result.error().code));
    EXPECT_FALSE(dns::detail::is_definitive(result.error().code));
}

// ---------------------------------------------------------------------------
// DoH: the catch-all and the per-address connect timeout
// ---------------------------------------------------------------------------

TEST(DohResolver, factoryDefectIsUnknownSoTheSearchFallsOver) {
    auto factory = std::make_shared<ThrowingFactory>();
    http::Options options;
    options.factory = factory;
    dns::DohResolver resolver{DOH_URL, options};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::UNKNOWN);
    EXPECT_TRUE(dns::detail::is_retryable(result.error().code));
    EXPECT_FALSE(dns::detail::is_definitive(result.error().code));
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
    // The one retry spent its own connect budget too: 2 × connect_timeout, far
    // below the 6s exchange budget that would otherwise have been waited out.
    EXPECT_EQ(factory->tls_calls, 2);
    EXPECT_EQ(clock.now() - start, 200ms);
}

// ---------------------------------------------------------------------------
// bootstrap server policy
// ---------------------------------------------------------------------------

TEST(BootstrapResolve, nonLiteralServerIsSkippedAndReportedAsConfig) {
    // No network: the only configured server is rejected before any query.
    const auto result = coro::run(dns::bootstrap_resolve("example.org", std::nullopt, {{"not-an-ip", 53}}));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result.error().message.find("not an IP literal"), std::string::npos);
}

}  // namespace