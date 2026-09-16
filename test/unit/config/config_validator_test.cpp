//
// Unit tests for config/validator.hpp — EnvironmentValidator.
//
// Environment-dependent checks only: every referenced driver must be loaded
// and every referenced interface must exist on this machine.
// Static checks live in static_validator.cpp and are covered by
// static_validator_test.cpp.
// =============================================================================

#include <initializer_list>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "config/validator.hpp"
#include "exception/config_verification.h"
#include "network/net_devices.h"

namespace {
    /// Return the given list of interfaces plus the platform's loopback name.
    /// This avoids hardcoding "lo" or "lo0".
    [[nodiscard]] std::vector<std::string> ifaces(std::initializer_list<std::string> list) {
        std::vector<std::string> result;
        result.reserve(list.size() + 1);
        result.push_back(NetDevices::loopback_name());
        result.insert(result.end(), list.begin(), list.end());
        return result;
    }

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
} // anonymous namespace

TEST(EnvironmentValidator, ValidConfig_Passes) {
    const EnvironmentValidator validator({"test_driver"}, {});
    EXPECT_NO_THROW(validator.validate(make_config()));
}

TEST(EnvironmentValidator, DriverNotFound_Throws) {
    const EnvironmentValidator validator({"some_other_driver"}, {});
    try {
        validator.validate(make_config());
        FAIL() << "Expected ConfigVerificationException";
    } catch (const ConfigVerificationException &e) {
        EXPECT_EQ(e.what(), std::string("Driver test_driver not found"));
    }
}

TEST(EnvironmentValidator, NoDriversLoaded_Throws) {
    const EnvironmentValidator validator({}, {});
    EXPECT_THROW(validator.validate(make_config()), ConfigVerificationException);
}

TEST(EnvironmentValidator, InterfaceExists_Passes) {
    const EnvironmentValidator validator({"test_driver"}, ifaces({"eth0"}));
    EXPECT_NO_THROW(validator.validate(make_config("test_driver", "eth0")));
}

TEST(EnvironmentValidator, InterfaceNotFound_Throws) {
    const auto available = ifaces({"eth1"});
    const EnvironmentValidator validator({"test_driver"}, available);
    try {
        validator.validate(make_config("test_driver", "eth0"));
        FAIL() << "Expected ConfigVerificationException";
    } catch (const ConfigVerificationException &e) {
        EXPECT_THAT(e.what(), testing::StartsWith("Interface eth0 not found, available interfaces: "));
    }
}

TEST(EnvironmentValidator, EmptyInterface_NotChecked) {
    // An empty interface field is not looked up (HTTP source without binding).
    const EnvironmentValidator validator({"test_driver"}, {});
    EXPECT_NO_THROW(validator.validate(make_config("test_driver", "")));
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

    const EnvironmentValidator ok({"drv1", "drv2"}, {});
    EXPECT_NO_THROW(ok.validate(config));

    const EnvironmentValidator missing({"drv1"}, {});
    EXPECT_THROW(missing.validate(config), ConfigVerificationException);
}
