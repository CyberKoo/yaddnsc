//
// Unit tests for application/environment_validator — the environment-dependent
// configuration checks (every referenced driver must be loaded, every
// referenced interface must exist) against fake catalog / interface ports.
//
// Static checks live in Config::validate_static and are covered by
// static_validator_test.cpp.
// =============================================================================

#include "application/environment_validator.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "application/ports/driver_catalog.h"
#include "domain/config/ip_source_kind.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "mocks/mock_ports.h"

namespace {
/// Minimal runtime config with one domain and one subdomain.
[[nodiscard]] domain::RuntimeConfig make_config(std::string driver = "test_driver", std::string interface = "") {
    domain::RuntimeConfig config;
    config.domains.push_back(domain::DomainConfig{
        .name = "example.com",
        .update_interval = 300,
        .force_update = 0,
        .driver = std::move(driver),
        .subdomains = {{
            domain::SubdomainConfig{
                .name = "www",
                .type = domain::RecordKind::A,
                .interface = std::move(interface),
                .ip_source = domain::IpSource::HTTP,
                .ip_source_param = "https://api.ipify.org",
                .update_interval = 300,
            },
        }},
    });
    return config;
}

/// A descriptor for @p name, publishing the given record kinds. The remaining
/// metadata is irrelevant to the validator but must be named for
/// -Wmissing-field-initializers.
[[nodiscard]] app::DriverDescription descriptor(std::string_view name,
                                                std::vector<domain::RecordKind> supported_records) {
    return app::DriverDescription{
        .name = std::string(name),
        .version = "1.0.0",
        .author = "test",
        .description = "test driver",
        .supported_records = std::move(supported_records),
        .supports_validate = true,
    };
}

/// Catalog fake reporting the given drivers as loaded.
/// (gmock mocks are immovable — hand out ownership instead.)
///
/// By default every listed driver publishes both ABI 1.0 record types, so the
/// record-type check stays neutral unless a test says otherwise; a later
/// ON_CALL for a specific name overrides this one.
[[nodiscard]] std::unique_ptr<MockDriverCatalogPort> catalog_with(std::vector<std::string> drivers) {
    auto catalog = std::make_unique<MockDriverCatalogPort>();
    ON_CALL(*catalog, loaded_drivers()).WillByDefault(::testing::Return(std::move(drivers)));
    ON_CALL(*catalog, describe(::testing::_))
        .WillByDefault(::testing::Invoke([](std::string_view name) {
            return std::expected<app::DriverDescription, domain::DriverError>{
                descriptor(name, {domain::RecordKind::A, domain::RecordKind::AAAA})};
        }));
    return catalog;
}

/// Interface fake reporting the given interface names.
[[nodiscard]] std::unique_ptr<MockNetworkInterfaces> interfaces_with(std::vector<std::string> names) {
    auto interfaces = std::make_unique<MockNetworkInterfaces>();
    ON_CALL(*interfaces, names()).WillByDefault(::testing::Return(std::move(names)));
    return interfaces;
}
}  // anonymous namespace

TEST(EnvironmentValidator, ValidConfig_Passes) {
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({});
    EXPECT_TRUE(app::validate_environment(make_config(), *catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, DriverNotFound_Fails) {
    auto catalog = catalog_with({"some_other_driver"});
    auto interfaces = interfaces_with({});
    const auto result = app::validate_environment(make_config(), *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(result.error().size(), 1);
    EXPECT_EQ(result.error().front().code, domain::ConfigError::Code::DRIVER_NOT_FOUND);
    EXPECT_EQ(result.error().front().message, "Driver test_driver not found");
}

TEST(EnvironmentValidator, DriverWithoutRecordCapability_Fails) {
    // The host enforces the capability bits before every update; reporting it
    // at startup turns a silent per-cycle failure into a configuration error.
    auto catalog = catalog_with({"test_driver"});
    ON_CALL(*catalog, describe("test_driver"))
        .WillByDefault(::testing::Return(descriptor("test_driver", {domain::RecordKind::AAAA})));
    auto interfaces = interfaces_with({});
    const auto result = app::validate_environment(make_config(), *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(result.error().size(), 1);
    EXPECT_EQ(result.error().front().code, domain::ConfigError::Code::DRIVER_UNSUPPORTED_RECORD);
    EXPECT_EQ(result.error().front().message,
              "Driver test_driver does not support record type A required by example.com.www");
}

TEST(EnvironmentValidator, DriverWithRecordCapability_Passes) {
    auto catalog = catalog_with({"test_driver"});
    ON_CALL(*catalog, describe("test_driver"))
        .WillByDefault(::testing::Return(descriptor("test_driver", {domain::RecordKind::A, domain::RecordKind::AAAA})));
    auto interfaces = interfaces_with({});
    EXPECT_TRUE(app::validate_environment(make_config(), *catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, DriverWithoutAnyCapability_FailsEveryRecord) {
    // Zero capabilities is legal at load (the plugin simply accepts no ABI 1.0
    // record type), so every configured record is a configuration error.
    auto catalog = catalog_with({"test_driver"});
    ON_CALL(*catalog, describe("test_driver")).WillByDefault(::testing::Return(descriptor("test_driver", {})));
    auto interfaces = interfaces_with({});
    const auto result = app::validate_environment(make_config(), *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().size(), 1);
    EXPECT_EQ(result.error().front().code, domain::ConfigError::Code::DRIVER_UNSUPPORTED_RECORD);
}

TEST(EnvironmentValidator, NoDriversLoaded_Fails) {
    auto catalog = catalog_with({});
    auto interfaces = interfaces_with({});
    EXPECT_FALSE(app::validate_environment(make_config(), *catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, InterfaceExists_Passes) {
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({"lo", "eth0"});
    EXPECT_TRUE(app::validate_environment(make_config("test_driver", "eth0"), *catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, InterfaceNotFound_Fails) {
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({"lo", "eth1"});
    const auto result = app::validate_environment(make_config("test_driver", "eth0"), *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(result.error().size(), 1);
    EXPECT_EQ(result.error().front().code, domain::ConfigError::Code::INTERFACE_NOT_FOUND);
    EXPECT_THAT(result.error().front().message,
                testing::StartsWith("Interface eth0 not found, available interfaces: "));
}

TEST(EnvironmentValidator, InterfaceNotFound_ListsAvailableInterfaces) {
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({"lo", "eth1"});
    const auto result = app::validate_environment(make_config("test_driver", "eth0"), *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().front().message, "Interface eth0 not found, available interfaces: lo, eth1");
}

TEST(EnvironmentValidator, EmptyInterface_NotChecked) {
    // An empty interface field is not looked up (HTTP source without binding).
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({});
    EXPECT_TRUE(app::validate_environment(make_config("test_driver", ""), *catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, MultipleDomains_EveryDriverMustBeLoaded) {
    auto config = make_config("drv1");
    config.domains.push_back(domain::DomainConfig{
        .name = "test.org",
        .update_interval = 120,
        .force_update = 0,
        .driver = "drv2",
        .subdomains = {{
            domain::SubdomainConfig{
                .name = "@",
                .type = domain::RecordKind::AAAA,
                .ip_source = domain::IpSource::HTTP,
                .ip_source_param = "https://api6.ipify.org",
                .update_interval = 120,
            },
        }},
    });

    auto ok_catalog = catalog_with({"drv1", "drv2"});
    auto interfaces = interfaces_with({});
    EXPECT_TRUE(app::validate_environment(config, *ok_catalog, *interfaces).has_value());

    auto missing_catalog = catalog_with({"drv1"});
    EXPECT_FALSE(app::validate_environment(config, *missing_catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, CollectsAllViolations_InConfigurationOrder) {
    // One startup should surface one broken configuration, not one mistake per
    // restart: a missing driver, a bad interface and an unsupported record type
    // are all reported, in configuration order.
    auto config = make_config("missing_one", "nope0");
    config.domains.push_back(domain::DomainConfig{
        .name = "test.org",
        .update_interval = 120,
        .force_update = 0,
        .driver = "loaded_but_limited",
        .subdomains = {{
            domain::SubdomainConfig{
                .name = "@",
                .type = domain::RecordKind::AAAA,
                .interface = "nope1",
                .ip_source = domain::IpSource::HTTP,
                .ip_source_param = "https://api6.ipify.org",
                .update_interval = 120,
            },
        }},
    });

    auto catalog = catalog_with({"loaded_but_limited"});
    ON_CALL(*catalog, describe("loaded_but_limited"))
        .WillByDefault(::testing::Return(descriptor("loaded_but_limited", {domain::RecordKind::A})));  // AAAA missing
    auto interfaces = interfaces_with({"lo"});
    const auto result = app::validate_environment(config, *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(result.error().size(), 4);
    EXPECT_EQ(result.error()[0].code, domain::ConfigError::Code::DRIVER_NOT_FOUND);
    EXPECT_EQ(result.error()[0].message, "Driver missing_one not found");
    EXPECT_EQ(result.error()[1].code, domain::ConfigError::Code::INTERFACE_NOT_FOUND);
    EXPECT_EQ(result.error()[1].message, "Interface nope0 not found, available interfaces: lo");
    EXPECT_EQ(result.error()[2].code, domain::ConfigError::Code::INTERFACE_NOT_FOUND);
    EXPECT_EQ(result.error()[2].message, "Interface nope1 not found, available interfaces: lo");
    EXPECT_EQ(result.error()[3].code, domain::ConfigError::Code::DRIVER_UNSUPPORTED_RECORD);
    EXPECT_EQ(result.error()[3].message,
              "Driver loaded_but_limited does not support record type AAAA required by test.org.@");
}
