#include "application/diagnostics.h"

#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <expected>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "application/ports/gateway.h"
#include "application/ports/resolver.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/update/driver_update_command.h"
#include "infrastructure/coro/coro.h"
#include "mocks/mock_ports.h"

namespace {
using namespace std::chrono_literals;
using DriverResult = std::expected<void, domain::DriverError>;
using LookupResult = std::expected<std::vector<std::string>, domain::DnsErrorInfo>;

class MockGateway : public app::GatewayPort {
public:
    MOCK_METHOD(coro::Task<DriverResult>, validate_config, (std::string, std::string), (override));
    MOCK_METHOD(coro::Task<DriverResult>, update, (std::string, domain::DriverUpdateCommand), (override));
};

class MockResolver : public app::ResolverPort {
public:
    MOCK_METHOD(coro::Task<LookupResult>, resolve, (std::string, domain::RecordKind), (override));
};

coro::Task<DriverResult> accepted() {
    co_return DriverResult{};
}

coro::Task<DriverResult> rejected() {
    co_return std::unexpected(domain::DriverError{domain::DriverError::Code::UPDATE_FAILED, "missing zone_id"});
}

coro::Task<LookupResult> hanging_lookup() {
    co_await coro::sleep_for(1h);
    co_return std::vector<std::string>{};
}

coro::Task<LookupResult> successful_lookup() {
    co_return std::vector<std::string>{"192.0.2.1"};
}

coro::Task<LookupResult> failed_lookup() {
    co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, "unavailable"});
}

coro::Task<LookupResult> defective_lookup() {
    throw std::runtime_error("defect");
    co_return std::vector<std::string>{};
}

domain::RuntimeConfig config() {
    domain::RuntimeConfig value;
    domain::DomainConfig domain;
    domain.name = "example.com";
    domain.driver = "fake";
    domain.subdomains = {
        domain::SubdomainConfig{.name = "@", .driver_params = "{}"},
        domain::SubdomainConfig{.name = "www", .driver_params = R"({"zone_id":"ok"})"},
    };
    value.domains.push_back(std::move(domain));
    return value;
}
}  // namespace

TEST(ApplicationDiagnostics, ValidateDriverConfigs_AllAccepted_ChecksEverySubdomain) {
    testing::StrictMock<MockGateway> gateway;
    auto runtime = config();
    auto other_domain = runtime.domains.front();
    other_domain.driver = "other";
    other_domain.subdomains.resize(1);
    runtime.domains.push_back(std::move(other_domain));
    testing::InSequence order;
    EXPECT_CALL(gateway, validate_config("fake", "{}")).WillOnce([] { return accepted(); });
    EXPECT_CALL(gateway, validate_config("fake", R"({"zone_id":"ok"})")).WillOnce([] { return accepted(); });
    EXPECT_CALL(gateway, validate_config("other", "{}")).WillOnce([] { return accepted(); });
    coro::Loop loop;
    EXPECT_TRUE(coro::run(loop, app::validate_driver_configs(gateway, runtime)));
}

TEST(ApplicationDiagnostics, ValidateDriverConfigs_Rejection_ReportsFqdnAndStops) {
    testing::StrictMock<MockGateway> gateway;
    const auto runtime = config();
    EXPECT_CALL(gateway, validate_config("fake", "{}")).WillOnce([] { return rejected(); });
    coro::Loop loop;
    const auto result = coro::run(loop, app::validate_driver_configs(gateway, runtime));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), "Driver 'fake' rejected configuration for example.com: missing zone_id");
}

TEST(ApplicationDiagnostics, DnsResolveCommand_Success_PreservesOutcome) {
    testing::StrictMock<MockResolver> resolver;
    EXPECT_CALL(resolver, resolve("example.com", domain::RecordKind::A)).WillOnce([] { return successful_lookup(); });
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto result = coro::run(loop, app::dns_resolve_command(resolver, "example.com", "a", 30s));
    EXPECT_EQ(result.host, "example.com");
    EXPECT_EQ(result.type_text, "a");
    ASSERT_TRUE(result.lookup);
    ASSERT_TRUE(*result.lookup);
    EXPECT_EQ(**result.lookup, std::vector<std::string>{"192.0.2.1"});
}

TEST(ApplicationDiagnostics, DnsResolveCommand_ResolverFailure_PreservesError) {
    testing::StrictMock<MockResolver> resolver;
    EXPECT_CALL(resolver, resolve("example.com", domain::RecordKind::A)).WillOnce([] { return failed_lookup(); });
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto result = coro::run(loop, app::dns_resolve_command(resolver, "example.com", "A", 30s));
    ASSERT_TRUE(result.lookup);
    ASSERT_FALSE(*result.lookup);
    EXPECT_EQ(result.lookup->error().message, "unavailable");
}

TEST(ApplicationDiagnostics, DnsResolveCommand_UnknownType_DoesNotCallResolver) {
    testing::StrictMock<MockResolver> resolver;
    coro::ManualClock clock;
    coro::Loop loop{clock};
    EXPECT_FALSE(coro::run(loop, app::dns_resolve_command(resolver, "example.com", "BOGUS", 30s)).lookup);
}

TEST(ApplicationDiagnostics, DnsResolveCommand_Timeout_ReportsBudget) {
    testing::StrictMock<MockResolver> resolver;
    EXPECT_CALL(resolver, resolve("example.com", domain::RecordKind::A)).WillOnce([] { return hanging_lookup(); });
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto start = clock.now();
    const auto result = coro::run(loop, app::dns_resolve_command(resolver, "example.com", "A", 2s));
    EXPECT_EQ(clock.now() - start, 2s);
    EXPECT_EQ(result.host, "example.com");
    EXPECT_EQ(result.type_text, "A");
    ASSERT_TRUE(result.lookup);
    ASSERT_FALSE(*result.lookup);
    EXPECT_EQ(result.lookup->error().code, domain::DnsError::CONNECTION);
    EXPECT_EQ(result.lookup->error().message, "DNS lookup timed out after 2s");
}

TEST(ApplicationDiagnostics, DnsResolveCommand_AncestorTimeout_PropagatesCancellation) {
    testing::StrictMock<MockResolver> resolver;
    EXPECT_CALL(resolver, resolve("example.com", domain::RecordKind::A)).WillOnce([] { return hanging_lookup(); });
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto result = coro::run(loop, coro::with_timeout(1s, [&resolver] {
                                      return app::dns_resolve_command(resolver, "example.com", "A", 30s);
                                  }));
    EXPECT_TRUE(result.timed_out);
    EXPECT_FALSE(result.has_value());
}

TEST(ApplicationDiagnostics, DnsResolveCommand_Defect_Propagates) {
    testing::StrictMock<MockResolver> resolver;
    EXPECT_CALL(resolver, resolve("example.com", domain::RecordKind::A)).WillOnce([] { return defective_lookup(); });
    coro::Loop loop;
    EXPECT_THROW(
        {
            [[maybe_unused]] const auto result =
                coro::run(loop, app::dns_resolve_command(resolver, "example.com", "A", 30s));
        },
        std::runtime_error);
}

TEST(ApplicationDiagnostics, ListDrivers_MissingDriver_CapturesError) {
    MockDriverCatalogPort catalog;
    EXPECT_CALL(catalog, loaded_drivers()).WillOnce(testing::Return(std::vector<std::string>{"missing"}));
    EXPECT_CALL(catalog, describe("missing"))
        .WillOnce(testing::Return(std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND, {}})));
    const auto items = app::list_drivers(catalog);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].name, "missing");
    ASSERT_FALSE(items[0].detail);
    EXPECT_EQ(items[0].detail.error().code, domain::DriverError::Code::NOT_FOUND);
}

TEST(ApplicationDiagnostics, ListDrivers_AllocationFailure_Propagates) {
    MockDriverCatalogPort catalog;
    EXPECT_CALL(catalog, loaded_drivers()).WillOnce(testing::Return(std::vector<std::string>{"driver"}));
    EXPECT_CALL(catalog, describe("driver")).WillOnce(testing::Throw(std::bad_alloc{}));
    EXPECT_THROW({ [[maybe_unused]] const auto items = app::list_drivers(catalog); }, std::bad_alloc);
}

TEST(ApplicationDiagnostics, ListDrivers_UnexpectedException_Propagates) {
    MockDriverCatalogPort catalog;
    EXPECT_CALL(catalog, loaded_drivers()).WillOnce(testing::Return(std::vector<std::string>{"driver"}));
    EXPECT_CALL(catalog, describe("driver")).WillOnce(testing::Throw(42));
    EXPECT_THROW({ [[maybe_unused]] const auto items = app::list_drivers(catalog); }, int);
}
