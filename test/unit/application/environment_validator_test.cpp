//
// Unit tests for application/environment_validator — the environment-dependent
// configuration checks (every referenced driver must be loaded, every
// referenced interface must exist) against fake catalog / interface ports.
//
// Static checks live in Config::validate_static and are covered by
// static_validator_test.cpp.
// =============================================================================

#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "application/environment_validator.h"

#include "mocks/mock_ports.h"

namespace {
    /// Minimal runtime config with one domain and one subdomain.
    [[nodiscard]] domain::RuntimeConfig make_config(std::string driver = "test_driver",
                                                    std::string interface = "") {
        domain::RuntimeConfig config;
        config.domains.push_back(domain::DomainConfig{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = std::move(driver),
            .subdomains = {{
                domain::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .interface = std::move(interface),
                    .ip_source = Config::IpSource::HTTP,
                    .ip_source_param = "https://api.ipify.org",
                    .update_interval = 300,
                },
            }},
        });
        return config;
    }

    /// Catalog fake reporting the given drivers as loaded.
    /// (gmock mocks are immovable — hand out ownership instead.)
    [[nodiscard]] std::unique_ptr<MockDriverCatalogPort> catalog_with(std::vector<std::string> drivers) {
        auto catalog = std::make_unique<MockDriverCatalogPort>();
        ON_CALL(*catalog, loaded_drivers()).WillByDefault(::testing::Return(std::move(drivers)));
        return catalog;
    }

    /// Interface fake reporting the given interface names.
    [[nodiscard]] std::unique_ptr<MockNetworkInterfaces> interfaces_with(std::vector<std::string> names) {
        auto interfaces = std::make_unique<MockNetworkInterfaces>();
        ON_CALL(*interfaces, names()).WillByDefault(::testing::Return(std::move(names)));
        return interfaces;
    }
} // anonymous namespace

TEST(EnvironmentValidator, ValidConfig_Passes) {
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({});
    EXPECT_TRUE(validate_environment(make_config(), *catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, DriverNotFound_Fails) {
    auto catalog = catalog_with({"some_other_driver"});
    auto interfaces = interfaces_with({});
    const auto result = validate_environment(make_config(), *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(result.error().size(), 1);
    EXPECT_EQ(result.error().front().code, domain::ConfigError::Code::DRIVER_NOT_FOUND);
    EXPECT_EQ(result.error().front().message, "Driver test_driver not found");
}

TEST(EnvironmentValidator, NoDriversLoaded_Fails) {
    auto catalog = catalog_with({});
    auto interfaces = interfaces_with({});
    EXPECT_FALSE(validate_environment(make_config(), *catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, InterfaceExists_Passes) {
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({"lo", "eth0"});
    EXPECT_TRUE(validate_environment(make_config("test_driver", "eth0"), *catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, InterfaceNotFound_Fails) {
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({"lo", "eth1"});
    const auto result = validate_environment(make_config("test_driver", "eth0"), *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(result.error().size(), 1);
    EXPECT_EQ(result.error().front().code, domain::ConfigError::Code::INTERFACE_NOT_FOUND);
    EXPECT_THAT(result.error().front().message,
                testing::StartsWith("Interface eth0 not found, available interfaces: "));
}

TEST(EnvironmentValidator, InterfaceNotFound_ListsAvailableInterfaces) {
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({"lo", "eth1"});
    const auto result = validate_environment(make_config("test_driver", "eth0"), *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().front().message, "Interface eth0 not found, available interfaces: lo, eth1");
}

TEST(EnvironmentValidator, EmptyInterface_NotChecked) {
    // An empty interface field is not looked up (HTTP source without binding).
    auto catalog = catalog_with({"test_driver"});
    auto interfaces = interfaces_with({});
    EXPECT_TRUE(validate_environment(make_config("test_driver", ""), *catalog, *interfaces).has_value());
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
                .type = RecordKind::AAAA,
                .ip_source = Config::IpSource::HTTP,
                .ip_source_param = "https://api6.ipify.org",
                .update_interval = 120,
            },
        }},
    });

    auto ok_catalog = catalog_with({"drv1", "drv2"});
    auto interfaces = interfaces_with({});
    EXPECT_TRUE(validate_environment(config, *ok_catalog, *interfaces).has_value());

    auto missing_catalog = catalog_with({"drv1"});
    EXPECT_FALSE(validate_environment(config, *missing_catalog, *interfaces).has_value());
}

TEST(EnvironmentValidator, FailFast_ReportsFirstViolation) {
    // Two domains, both referencing missing drivers: only the first is reported.
    auto config = make_config("missing_one");
    config.domains.push_back(domain::DomainConfig{
        .name = "test.org",
        .update_interval = 120,
        .force_update = 0,
        .driver = "missing_two",
        .subdomains = {},
    });

    auto catalog = catalog_with({});
    auto interfaces = interfaces_with({});
    const auto result = validate_environment(config, *catalog, *interfaces);
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(result.error().size(), 1);
    EXPECT_EQ(result.error().front().message, "Driver missing_one not found");
}
