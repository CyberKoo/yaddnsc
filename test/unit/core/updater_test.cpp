//
// Updater unit tests — exercises the single-task update pipeline using injected
// mocks: MockResolver (via ResolverDispatcher), a fake IpSourceBase, MockDriver,
// and MockHttpClient.
//

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <glaze/glaze.hpp>

#include "core/updater.h"
#include "core/update_task.h"
#include "dns/dispatcher.h"

#include "interface/driver.h"
#include "ip_source/base.h"
#include "network/inet_address.h"

#include "config/config.h"
#include "config/parser.hpp"

#include "fmt.hpp"

#include "fixtures/sample_config.h"
#include "mocks/mock_driver.h"
#include "mocks/mock_http_client.h"
#include "mocks/mock_resolver.h"

namespace {

using ::testing::_;
using ::testing::Return;

// ── Fake IP source ────────────────────────────────────────────────────────────

class FakeIpSource : public IpSourceBase {
public:
    explicit FakeIpSource(std::vector<InetAddress> addrs)
        : addrs_(std::make_shared<std::vector<InetAddress>>(std::move(addrs))) {}

    // IpSourceBase is non-copyable (NoCopy), so provide a copy constructor
    // that shares the underlying address list instead of copying the base.
    FakeIpSource(const FakeIpSource &other) noexcept : addrs_(other.addrs_) {}

    std::vector<InetAddress> resolve() const override { return *addrs_; }

private:
    std::shared_ptr<std::vector<InetAddress>> addrs_;
};

// Factory that returns the shared FakeIpSource regardless of config.
class FakeIpSourceFactory {
public:
    explicit FakeIpSourceFactory(std::shared_ptr<FakeIpSource> src) : src_(std::move(src)) {}

    std::unique_ptr<IpSourceBase> operator()(const Config::SubdomainConfig &) const {
        return std::make_unique<FakeIpSource>(*src_);
    }

private:
    std::shared_ptr<FakeIpSource> src_;
};

// ── Helpers ─────────────────────────────────────────────────────────────────

[[nodiscard]] Config::AppConfig parse_cfg(std::string_view json) {
    Config::AppConfig cfg{};
    const auto ec = glz::read<glz::opts{.error_on_missing_keys = false}>(cfg, json);
    EXPECT_EQ(ec, glz::error_code::none) << glz::format_error(ec, json);
    return cfg;
}

// Build a single-subdomain task from the fixture config.
[[nodiscard]] UpdateTask make_task(const Config::AppConfig &cfg, std::size_t domain_idx = 0,
                                   std::size_t sub_idx = 0) {
    const auto &domain = cfg.domains[domain_idx];
    const auto &sub = domain.subdomains[sub_idx];
    return UpdateTask{
        .config = sub,
        .domain_name = domain.name,
        .driver_name = domain.driver,
        .fqdn = fmt::format("{}.{}", sub.name, domain.name),
        .force_update = false,
    };
}

// A MockResolver that returns a fixed A record (192.0.2.1) for any query.
class FixedAResolver : public MockResolver {
public:
    FixedAResolver() {
        ON_CALL(*this, query(_, _, _))
            .WillByDefault(Return(std::vector<std::uint8_t>{
                0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
                0x00, 0x01, 0x00, 0x01, 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                0x00, 0x00, 0x01, 0x2C, 0x00, 0x04, 0xC0, 0x00, 0x02, 0x01}));
        ON_CALL(*this, get_type()).WillByDefault(Return("Mock"));
    }
};

// A MockResolver that always fails (NXDOMAIN-style error).
class FailingResolver : public MockResolver {
public:
    FailingResolver() {
        ON_CALL(*this, query(_, _, _))
            .WillByDefault(Return(std::unexpected(DnsErrorInfo{
                DnsError::NX_DOMAIN, "domain does not exist"})));
        ON_CALL(*this, get_type()).WillByDefault(Return("Mock"));
    }
};

// Build a ResolverDispatcher from a single resolver (takes ownership).
// IMPORTANT: the returned dispatcher is stored by *non-owning reference* inside
// Updater, so callers must keep the returned object alive for the whole test.
[[nodiscard]] ResolverDispatcher make_dispatcher(std::unique_ptr<ResolverBase> resolver) {
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(resolver));
    return ResolverDispatcher(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);
}

// A successful HTTP exchange returning 200.
HttpResult ok_response() {
    return HttpResponse{.status_code = 200, .body = "ok"};
}

} // namespace

// ── IP unchanged → driver not invoked ─────────────────────────────────────────

TEST(Updater, SkipsUpdateWhenIpUnchanged) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    auto ip = std::make_shared<FakeIpSource>(
        std::vector<InetAddress>{Inet4Address::from_bytes({192, 0, 2, 1})});
    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(driver, generate_request).Times(0);
    EXPECT_CALL(http, exchange).Times(0);

    updater.process(task, driver, http);
}

// ── IP changed → driver invoked ───────────────────────────────────────────────

TEST(Updater, UpdatesWhenIpChanged) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    // Local IP differs from the DNS record (192.0.2.1), so an update is sent.
    auto ip = std::make_shared<FakeIpSource>(
        std::vector<InetAddress>{Inet4Address::from_bytes({198, 51, 100, 1})});
    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(http, exchange).WillOnce(Return(ok_response()));
    EXPECT_CALL(driver, generate_request).WillOnce(Return(DriverRequestContext{.url = "https://api.example.com/update", .request = {}}));
    EXPECT_CALL(driver, check_response).WillOnce(Return(true));

    updater.process(task, driver, http);
}

// ── force_update → DNS comparison skipped ─────────────────────────────────────

TEST(Updater, ForceUpdateSkipsDnsComparison) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);
    task.force_update = true;

    auto ip = std::make_shared<FakeIpSource>(
        std::vector<InetAddress>{Inet4Address::from_bytes({192, 0, 2, 1})});
    // Even though the IP equals the DNS record, force_update must still update.
    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(http, exchange).WillOnce(Return(ok_response()));
    EXPECT_CALL(driver, generate_request).WillOnce(Return(DriverRequestContext{.url = "https://api.example.com/update", .request = {}}));
    EXPECT_CALL(driver, check_response).WillOnce(Return(true));

    updater.process(task, driver, http);
}

// ── empty IP source → update skipped ──────────────────────────────────────────

TEST(Updater, SkipsWhenIpSourceReturnsEmpty) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    auto ip = std::make_shared<FakeIpSource>(std::vector<InetAddress>{});
    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(driver, generate_request).Times(0);
    EXPECT_CALL(http, exchange).Times(0);

    updater.process(task, driver, http);
}

// ── DNS lookup failure → still attempts update ────────────────────────────────

TEST(Updater, UpdatesWhenDnsLookupFails) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    auto ip = std::make_shared<FakeIpSource>(
        std::vector<InetAddress>{Inet4Address::from_bytes({198, 51, 100, 1})});
    // Resolver fails → records empty → comparison skipped → update attempted.
    auto dispatcher = make_dispatcher(std::make_unique<FailingResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(http, exchange).WillOnce(Return(ok_response()));
    EXPECT_CALL(driver, generate_request).WillOnce(Return(DriverRequestContext{.url = "https://api.example.com/update", .request = {}}));
    EXPECT_CALL(driver, check_response).WillOnce(Return(true));

    updater.process(task, driver, http);
}

// ── driver.execute returns false → no throw (noexcept boundary) ───────────────

TEST(Updater, NoThrowWhenDriverReportsFailure) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    auto ip = std::make_shared<FakeIpSource>(
        std::vector<InetAddress>{Inet4Address::from_bytes({198, 51, 100, 1})});
    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(http, exchange).WillOnce(Return(ok_response()));
    EXPECT_CALL(driver, generate_request).WillOnce(Return(DriverRequestContext{.url = "https://api.example.com/update", .request = {}}));
    EXPECT_CALL(driver, check_response).WillOnce(Return(false));

    EXPECT_NO_THROW(updater.process(task, driver, http));
}

// ── AAAA + link-local filtering ───────────────────────────────────────────────

TEST(Updater, FiltersLinkLocalForAaaaWhenNotAllowed) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    // The "www" subdomain is type AAAA, interface source, allow_local_link=false.
    auto task = make_task(cfg, 0, 1);

    // Only a link-local candidate is available; it must be filtered out.
    auto ip = std::make_shared<FakeIpSource>(std::vector<InetAddress>{
        Inet6Address::from_bytes({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})});
    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(driver, generate_request).Times(0);
    EXPECT_CALL(http, exchange).Times(0);

    updater.process(task, driver, http);
}

TEST(Updater, KeepsLinkLocalForAaaaWhenAllowed) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg, 0, 1);
    task.config.allow_local_link = true; // override

    auto ip = std::make_shared<FakeIpSource>(std::vector<InetAddress>{
        Inet6Address::from_bytes({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})});
    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(http, exchange).WillOnce(Return(ok_response()));
    EXPECT_CALL(driver, generate_request).WillOnce(Return(DriverRequestContext{.url = "https://api.example.com/update", .request = {}}));
    EXPECT_CALL(driver, check_response).WillOnce(Return(true));

    updater.process(task, driver, http);
}

// ── IP source throws → swallowed at noexcept boundary ─────────────────────────

// ── AAAA + ULA filtering ─────────────────────────────────────────────────────

TEST(Updater, FiltersUlaForAaaaWhenNotAllowed) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg, 0, 1);

    // Only a ULA candidate is available; it must be filtered out.
    auto ip = std::make_shared<FakeIpSource>(std::vector<InetAddress>{
        Inet6Address::from_bytes({0xfc, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})});
    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(driver, generate_request).Times(0);
    EXPECT_CALL(http, exchange).Times(0);

    updater.process(task, driver, http);
}

TEST(Updater, KeepsUlaForAaaaWhenAllowed) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg, 0, 1);
    task.config.allow_ula = true; // override

    auto ip = std::make_shared<FakeIpSource>(std::vector<InetAddress>{
        Inet6Address::from_bytes({0xfc, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})});
    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, FakeIpSourceFactory(ip));

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(http, exchange).WillOnce(Return(ok_response()));
    EXPECT_CALL(driver, generate_request).WillOnce(Return(DriverRequestContext{.url = "https://api.example.com/update", .request = {}}));
    EXPECT_CALL(driver, check_response).WillOnce(Return(true));

    updater.process(task, driver, http);
}

TEST(Updater, NoThrowWhenIpSourceThrows) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    // A factory that returns a source which throws on resolve().
    class ThrowingIpSource : public IpSourceBase {
    public:
        std::vector<InetAddress> resolve() const override {
            throw std::runtime_error("interface not found");
        }
    };
    auto factory = [](const Config::SubdomainConfig &) {
        return std::make_unique<ThrowingIpSource>();
    };

    auto dispatcher = make_dispatcher(std::make_unique<FixedAResolver>());
    Updater updater(dispatcher, factory);

    MockDriver driver;
    MockHttpClient http;
    EXPECT_CALL(driver, generate_request).Times(0);

    EXPECT_NO_THROW(updater.process(task, driver, http));
}
