#include "application/diagnostics.h"

#include <chrono>
#include <coroutine>
#include <expected>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "application/config_validation.h"
#include "application/ports/driver_catalog.h"
#include "application/ports/gateway.h"
#include "application/ports/network_interfaces.h"
#include "application/ports/resolver.h"
#include "coro/coro.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "mocks/mock_ports.h"

namespace domain {
struct DriverUpdateCommand;
}  // namespace domain

namespace {
using namespace std::chrono_literals;
using DriverResult = std::expected<void, domain::DriverError>;
using LookupResult = std::expected<std::vector<std::string>, domain::DnsErrorInfo>;

class MockGateway : public app::GatewayPort {
public:
    MOCK_METHOD(bool, supports_validate, (std::string_view), (const, override));
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
    // One capability query per domain; declared before InSequence so it does
    // not have to interleave with the per-record validation calls.
    EXPECT_CALL(gateway, supports_validate).Times(2).WillRepeatedly(testing::Return(true));
    testing::InSequence order;
    EXPECT_CALL(gateway, validate_config("fake", "{}")).WillOnce([] { return accepted(); });
    EXPECT_CALL(gateway, validate_config("fake", R"({"zone_id":"ok"})")).WillOnce([] { return accepted(); });
    EXPECT_CALL(gateway, validate_config("other", "{}")).WillOnce([] { return accepted(); });
    coro::Loop loop;
    const auto outcome =
        coro::run(loop, app::validate_driver_configs(gateway, runtime, app::ConfigValidationMode::WARN_AND_CONTINUE, 5s));
    EXPECT_TRUE(outcome.rejection.empty());
    EXPECT_TRUE(outcome.unchecked_drivers.empty());
}

TEST(ApplicationDiagnostics, ValidateDriverConfigs_Rejection_ReportsFqdnAndStops) {
    testing::StrictMock<MockGateway> gateway;
    const auto runtime = config();
    EXPECT_CALL(gateway, supports_validate).WillRepeatedly(testing::Return(true));
    EXPECT_CALL(gateway, validate_config("fake", "{}")).WillOnce([] { return rejected(); });
    coro::Loop loop;
    const auto outcome =
        coro::run(loop, app::validate_driver_configs(gateway, runtime, app::ConfigValidationMode::WARN_AND_CONTINUE, 5s));
    EXPECT_EQ(outcome.rejection, "Driver 'fake' rejected configuration for example.com: missing zone_id");
}

TEST(ApplicationDiagnostics, ValidateDriverConfigs_DuplicateDriverParams_ValidatedOnce) {
    // Records sharing an identical (driver, driver_params) pair get one plugin
    // round-trip; the duplicate reuses the verdict. StrictMock turns a second
    // call into a failure.
    testing::StrictMock<MockGateway> gateway;
    auto runtime = config();
    runtime.domains.front().subdomains = {
        domain::SubdomainConfig{.name = "@", .driver_params = "{}"},
        domain::SubdomainConfig{.name = "www", .driver_params = "{}"},
        domain::SubdomainConfig{.name = "mail", .driver_params = R"({"zone_id":"ok"})"},
    };
    EXPECT_CALL(gateway, supports_validate).WillRepeatedly(testing::Return(true));
    EXPECT_CALL(gateway, validate_config("fake", "{}")).WillOnce([] { return accepted(); });
    EXPECT_CALL(gateway, validate_config("fake", R"({"zone_id":"ok"})")).WillOnce([] { return accepted(); });
    coro::Loop loop;
    const auto outcome =
        coro::run(loop, app::validate_driver_configs(gateway, runtime, app::ConfigValidationMode::WARN_AND_CONTINUE, 5s));
    EXPECT_TRUE(outcome.rejection.empty());
    EXPECT_TRUE(outcome.unchecked_drivers.empty());
}

TEST(ApplicationDiagnostics, ValidateDriverConfigs_RequireValidation_TreatsMissingEntryAsFailure) {
    // `config test` exists to answer whether the configuration is good, so a
    // driver whose parameters cannot be checked is not an answer.
    testing::StrictMock<MockGateway> gateway;
    const auto runtime = config();
    EXPECT_CALL(gateway, supports_validate("fake")).WillOnce(testing::Return(false));
    coro::Loop loop;
    const auto outcome =
        coro::run(loop, app::validate_driver_configs(gateway, runtime, app::ConfigValidationMode::REQUIRE_VALIDATION, 5s));
    EXPECT_EQ(outcome.rejection,
              "Driver 'fake' does not provide yaddnsc_driver_validate; its configuration was not checked");
}

TEST(ApplicationDiagnostics, ValidateDriverConfigs_WarnAndContinue_SkipsDriverWithoutEntry) {
    // The opposite policy, for `run`: a plugin built against an older ABI
    // must not be able to stop a daemon that would otherwise update fine.
    testing::StrictMock<MockGateway> gateway;
    auto runtime = config();
    auto other_domain = runtime.domains.front();
    other_domain.driver = "legacy";
    other_domain.subdomains.resize(1);
    runtime.domains.push_back(std::move(other_domain));
    EXPECT_CALL(gateway, supports_validate("fake")).WillRepeatedly(testing::Return(true));
    EXPECT_CALL(gateway, supports_validate("legacy")).WillRepeatedly(testing::Return(false));
    EXPECT_CALL(gateway, validate_config("fake", "{}")).WillOnce([] { return accepted(); });
    EXPECT_CALL(gateway, validate_config("fake", R"({"zone_id":"ok"})")).WillOnce([] { return accepted(); });
    coro::Loop loop;
    const auto outcome =
        coro::run(loop, app::validate_driver_configs(gateway, runtime, app::ConfigValidationMode::WARN_AND_CONTINUE, 5s));
    EXPECT_TRUE(outcome.rejection.empty());
    EXPECT_THAT(outcome.unchecked_drivers, testing::ElementsAre("legacy"));
}

TEST(ApplicationDiagnostics, ValidateDriverConfigs_Timeout_RejectsAndNamesTheRecord) {
    testing::StrictMock<MockGateway> gateway;
    const auto runtime = config();
    EXPECT_CALL(gateway, supports_validate).WillRepeatedly(testing::Return(true));
    EXPECT_CALL(gateway, validate_config("fake", "{}")).WillOnce([]() -> coro::Task<DriverResult> {
        co_await coro::sleep_for(3600s);
        co_return DriverResult{};
    });
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const auto outcome =
        coro::run(loop, app::validate_driver_configs(gateway, runtime, app::ConfigValidationMode::WARN_AND_CONTINUE, 5s));
    EXPECT_EQ(outcome.rejection, "Driver 'fake' did not finish validating the configuration for example.com within 5s");
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

namespace {
/// Coroutine resolver double for the plain `dns resolve` handler: no gmock
/// needed, the handler's contract is "parse the type, pass the value through".
class FakeResolverPort final : public app::ResolverPort {
public:
    std::vector<std::string> records;
    std::optional<domain::DnsErrorInfo> error;

    coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> resolve(std::string,
                                                                                      domain::RecordKind) override {
        if (error.has_value()) {
            co_return std::unexpected(*error);
        }
        co_return records;
    }
};

[[nodiscard]] app::DnsResolveOutcome run_dns_resolve(app::ResolverPort& resolver, const std::string& host,
                                                     const std::string& type) {
    coro::Loop loop;
    return coro::run(loop, app::dns_resolve(resolver, host, type));
}
}  // namespace

TEST(ApplicationDiagnostics, DnsResolve_UnknownType_ReturnsNoLookup) {
    FakeResolverPort resolver;
    const auto outcome = run_dns_resolve(resolver, "example.com", "BOGUS");

    EXPECT_EQ(outcome.host, "example.com");
    EXPECT_EQ(outcome.type_text, "BOGUS");
    EXPECT_FALSE(outcome.lookup.has_value());
}

TEST(ApplicationDiagnostics, DnsResolve_TypeIsCaseInsensitive) {
    FakeResolverPort resolver;
    resolver.records = {"::1"};

    const auto outcome = run_dns_resolve(resolver, "example.com", "aaaa");
    ASSERT_TRUE(outcome.lookup.has_value());
    ASSERT_TRUE(outcome.lookup->has_value());
    EXPECT_EQ((*outcome.lookup)->front(), "::1");
}

TEST(ApplicationDiagnostics, DnsResolve_ErrorPassesThrough) {
    FakeResolverPort resolver;
    resolver.error = domain::DnsErrorInfo{domain::DnsError::NX_DOMAIN, "nxdomain"};

    const auto outcome = run_dns_resolve(resolver, "example.com", "A");
    ASSERT_TRUE(outcome.lookup.has_value());
    ASSERT_FALSE(outcome.lookup->has_value());
    EXPECT_EQ(outcome.lookup->error().message, "nxdomain");
}

TEST(ApplicationDiagnostics, ListInterfaces_CollectsAddresses) {
    MockNetworkInterfaces interfaces;
    ON_CALL(interfaces, list())
        .WillByDefault(::testing::Return(std::vector<app::InterfaceInfo>{
            {.name = "lo", .addresses = {domain::InetAddress(*domain::Inet4Address::parse("127.0.0.1"))}},
            {.name = "eth0", .addresses = {}},
        }));

    const auto items = app::list_interfaces(interfaces);
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0].name, "lo");
    EXPECT_EQ(items[0].addresses.size(), 1);
    EXPECT_EQ(items[1].name, "eth0");
    EXPECT_TRUE(items[1].addresses.empty());
}

TEST(ApplicationDiagnostics, DnsResolveCommand_SelfCancelledPort_PropagatesCancellation) {
    testing::StrictMock<MockResolver> resolver;
    EXPECT_CALL(resolver, resolve("example.com", domain::RecordKind::A))
        .WillOnce([](std::string, domain::RecordKind) -> coro::Task<LookupResult> {
            (co_await coro::current_scope()).cancel();
            co_await coro::checkpoint();
            co_return std::vector<std::string>{};
        });
    EXPECT_THROW(
        { [[maybe_unused]] auto outcome = coro::run(app::dns_resolve_command(resolver, "example.com", "A", 30s)); },
        coro::Cancelled);
}
