//
// Unit tests for config/validator.hpp — ConfigValidator + detail functions.
//
// This test targets the pure-logic detail functions in the detail namespace.
// Full ConfigValidator::validate() requires a DriverManager instance with
// loaded drivers, which is not available in the pure-unit-test layer
// (requires dlopen).  Those integration-level tests belong in test/driver/
// or test/integration/ once the infrastructure is set up.
//
// Verified:
//   - detail::fqdn_for — correct FQDN construction.
//   - detail::validate_ip_source — all four IP source branches:
//       INTERFACE, HTTP (valid/invalid URL), MDNS (valid/invalid domain,
//       .local suffix, non-A/AAAA type).
//   - detail::validate_resolver_address — DoH/DoT URIs, plain IPs,
//       invalid addresses.
//   - ConfigValidator::validate — parameterized tests covering driver
//       availability, update intervals, resolver addresses, and interface
//       existence checks.
// =============================================================================

#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "config/validator.hpp"
#include "mocks/mock_driver_manager.h"
#include "network/net_devices.h"

// ===========================================================================
// detail::fqdn_for
// ===========================================================================

TEST(ConfigValidatorDetailTest, FqdnFor_SubdomainAndDomain) {
    Config::DomainConfig domain{.name = "example.com"};
    Config::SubdomainConfig sub{.name = "www"};

    auto fqdn = detail::fqdn_for(domain, sub);
    EXPECT_EQ(fqdn, "www.example.com");
}

TEST(ConfigValidatorDetailTest, FqdnFor_ApexSubdomain) {
    Config::DomainConfig domain{.name = "example.com"};
    Config::SubdomainConfig sub{.name = "@"};

    auto fqdn = detail::fqdn_for(domain, sub);
    EXPECT_EQ(fqdn, "@.example.com");
}

TEST(ConfigValidatorDetailTest, FqdnFor_DeepSubdomain) {
    Config::DomainConfig domain{.name = "example.com"};
    Config::SubdomainConfig sub{.name = "a.b.c"};

    auto fqdn = detail::fqdn_for(domain, sub);
    EXPECT_EQ(fqdn, "a.b.c.example.com");
}

// ===========================================================================
// detail::validate_ip_source — INTERFACE
// ===========================================================================
// NOTE: These tests validate the validation logic, not the actual IP
// resolution.  IP resolution requires real network interfaces or mocks.

TEST(ConfigValidatorDetailTest, ValidateIpSource_Interface_WithInterface_Ok) {
    Config::SubdomainConfig sub{
        .name = "www",
        .type = RecordKind::A,
        .interface = "eth0",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::INTERFACE,
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_NO_THROW(detail::validate_ip_source(domain, sub));
}

TEST(ConfigValidatorDetailTest, ValidateIpSource_Interface_EmptyInterface_Throws) {
    Config::SubdomainConfig sub{
        .name = "www",
        .type = RecordKind::A,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::INTERFACE,
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_THROW(detail::validate_ip_source(domain, sub), ConfigVerificationException);
}

// ===========================================================================
// detail::validate_ip_source — HTTP
// ===========================================================================

TEST(ConfigValidatorDetailTest, ValidateIpSource_Http_ValidUrl_Ok) {
    Config::SubdomainConfig sub{
        .name = "www",
        .type = RecordKind::A,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::HTTP,
        .ip_source_param = "https://api.ipify.org",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_NO_THROW(detail::validate_ip_source(domain, sub));
}

TEST(ConfigValidatorDetailTest, ValidateIpSource_Http_EmptyParam_Throws) {
    Config::SubdomainConfig sub{
        .name = "www",
        .type = RecordKind::A,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::HTTP,
        .ip_source_param = "",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_THROW(detail::validate_ip_source(domain, sub), ConfigVerificationException);
}

TEST(ConfigValidatorDetailTest, ValidateIpSource_Http_InvalidUrl_Throws) {
    Config::SubdomainConfig sub{
        .name = "www",
        .type = RecordKind::A,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::HTTP,
        .ip_source_param = "not-a-url",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_THROW(detail::validate_ip_source(domain, sub), ConfigVerificationException);
}

TEST(ConfigValidatorDetailTest, ValidateIpSource_Http_MissingHost_Throws) {
    Config::SubdomainConfig sub{
        .name = "www",
        .type = RecordKind::A,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::HTTP,
        .ip_source_param = "http://:8080/path",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_THROW(detail::validate_ip_source(domain, sub), ConfigVerificationException);
}

// ===========================================================================
// detail::validate_ip_source — mDNS
// ===========================================================================

TEST(ConfigValidatorDetailTest, ValidateIpSource_Mdns_ValidLocalDomain_Ok) {
    Config::SubdomainConfig sub{
        .name = "printer",
        .type = RecordKind::A,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::MDNS,
        .ip_source_param = "printer.local",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_NO_THROW(detail::validate_ip_source(domain, sub));
}

TEST(ConfigValidatorDetailTest, ValidateIpSource_Mdns_TrailingDot_Ok) {
    Config::SubdomainConfig sub{
        .name = "printer",
        .type = RecordKind::AAAA,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::MDNS,
        .ip_source_param = "printer.local.",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_NO_THROW(detail::validate_ip_source(domain, sub));
}

TEST(ConfigValidatorDetailTest, ValidateIpSource_Mdns_EmptyParam_Throws) {
    Config::SubdomainConfig sub{
        .name = "printer",
        .type = RecordKind::A,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::MDNS,
        .ip_source_param = "",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_THROW(detail::validate_ip_source(domain, sub), ConfigVerificationException);
}

TEST(ConfigValidatorDetailTest, ValidateIpSource_Mdns_InvalidDomain_Throws) {
    Config::SubdomainConfig sub{
        .name = "printer",
        .type = RecordKind::A,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::MDNS,
        .ip_source_param = "not valid .local",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_THROW(detail::validate_ip_source(domain, sub), ConfigVerificationException);
}

TEST(ConfigValidatorDetailTest, ValidateIpSource_Mdns_NonLocalSuffix_Throws) {
    Config::SubdomainConfig sub{
        .name = "printer",
        .type = RecordKind::A,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::MDNS,
        .ip_source_param = "printer.example.com",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_THROW(detail::validate_ip_source(domain, sub), ConfigVerificationException);
}

TEST(ConfigValidatorDetailTest, ValidateIpSource_Mdns_TxtType_Throws) {
    Config::SubdomainConfig sub{
        .name = "printer",
        .type = RecordKind::TXT,
        .interface = "",
        .ip_type = AddressFamily::UNSPECIFIED,
        .ip_source = Config::IpSource::MDNS,
        .ip_source_param = "printer.local",
    };

    Config::DomainConfig domain{.name = "example.com"};
    EXPECT_THROW(detail::validate_ip_source(domain, sub), ConfigVerificationException);
}

// ===========================================================================
// detail::validate_resolver_address
// ===========================================================================

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_DoH_Ok) {
    EXPECT_NO_THROW(detail::validate_resolver_address("https://dns.cloudflare.com/dns-query"));
}

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_DoT_Ok) {
    EXPECT_NO_THROW(detail::validate_resolver_address("tls://1.1.1.1:853"));
}

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_IPv4_Ok) {
    EXPECT_NO_THROW(detail::validate_resolver_address("8.8.8.8"));
}

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_IPv6_Ok) {
    EXPECT_NO_THROW(detail::validate_resolver_address("::1"));
    EXPECT_NO_THROW(detail::validate_resolver_address("2001:db8::1"));
}

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_InvalidIP_Throws) {
    EXPECT_THROW(detail::validate_resolver_address("999.999.999.999"), ConfigVerificationException);
}

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_EmptyString_Throws) {
    EXPECT_THROW(detail::validate_resolver_address(""), ConfigVerificationException);
}

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_Hostname_Throws) {
    EXPECT_THROW(detail::validate_resolver_address("resolver.example.com"), ConfigVerificationException);
}

// ===========================================================================
// ConfigValidator::validate()  —  parameterized tests with MockDriverManager
// ===========================================================================

using Validator60 = ConfigValidator<60>;
using Validator300 = ConfigValidator<300>;

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Build a minimal AppConfig with one domain and one subdomain.
static Config::AppConfig make_domain_config(
    std::string domain_name = "example.com",
    int update_interval = 300,
    std::string driver_name = "test_driver",
    std::string subdomain_name = "www"
) {
    return Config::AppConfig{
        .driver = {},
        .resolver = {},
        .domains = {{
            .name = std::move(domain_name),
            .update_interval = update_interval,
            .force_update = 0,
            .driver = std::move(driver_name),
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = std::move(subdomain_name),
                    .type = RecordKind::A,
                    .interface = "",
                    .ip_type = AddressFamily::UNSPECIFIED,
                    .ip_source = Config::IpSource::HTTP,
                    .ip_source_param = "https://api.ipify.org",
                }
            }},
        }},
    };
}

// ── Config builder functions for parameterized test cases ───────────────────

namespace {

Config::AppConfig build_default() {
    return make_domain_config();
}

Config::AppConfig build_multi_domain() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {},
        .domains = {
            Config::DomainConfig{
                .name = "example.com",
                .update_interval = 300,
                .force_update = 0,
                .driver = "drv1",
                .subdomains = {{
                    Config::SubdomainConfig{
                        .name = "www",
                        .type = RecordKind::A,
                        .ip_source = Config::IpSource::HTTP,
                        .ip_source_param = "https://api.ipify.org",
                    },
                }},
            },
            Config::DomainConfig{
                .name = "test.org",
                .update_interval = 120,
                .force_update = 0,
                .driver = "drv2",
                .subdomains = {{
                    Config::SubdomainConfig{
                        .name = "@",
                        .type = RecordKind::AAAA,
                        .ip_source = Config::IpSource::HTTP,
                        .ip_source_param = "https://api6.ipify.org",
                    },
                }},
            },
        },
    };
}

Config::AppConfig build_empty_domain_name() {
    return make_domain_config("");
}

Config::AppConfig build_no_subdomains() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {},
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = "test_driver",
            .subdomains = {},
        }},
    };
}

Config::AppConfig build_empty_subdomain_name() {
    return make_domain_config("example.com", 300, "test_driver", "");
}

Config::AppConfig build_low_update_interval() {
    return make_domain_config("example.com", 60);
}

Config::AppConfig build_subdomain_low_interval() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {},
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = "test_driver",
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .interface = "",
                    .ip_type = AddressFamily::UNSPECIFIED,
                    .ip_source = Config::IpSource::HTTP,
                    .ip_source_param = "https://api.ipify.org",
                    .allow_ula = false,
                    .allow_local_link = false,
                    .update_interval = 10,
                }
            }},
        }},
    };
}

Config::AppConfig build_subdomain_ok_interval() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {},
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = "test_driver",
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .interface = "",
                    .ip_type = AddressFamily::UNSPECIFIED,
                    .ip_source = Config::IpSource::HTTP,
                    .ip_source_param = "https://api.ipify.org",
                    .allow_ula = false,
                    .allow_local_link = false,
                    .update_interval = 120,
                }
            }},
        }},
    };
}

Config::AppConfig build_force_update_less_than_update() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {},
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 30,
            .driver = "test_driver",
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .ip_source = Config::IpSource::HTTP,
                    .ip_source_param = "https://api.ipify.org",
                },
            }},
        }},
    };
}

Config::AppConfig build_force_update_disabled() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {},
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = "test_driver",
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .ip_source = Config::IpSource::HTTP,
                    .ip_source_param = "https://api.ipify.org",
                },
            }},
        }},
    };
}

Config::AppConfig build_custom_resolver_doh() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {
            .use_custom_server = true,
            .servers = {{
                Config::DnsServer{.address = "https://dns.cloudflare.com/dns-query", .port = 443},
            }},
        },
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = "test_driver",
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .ip_source = Config::IpSource::HTTP,
                    .ip_source_param = "https://api.ipify.org",
                },
            }},
        }},
    };
}

Config::AppConfig build_custom_resolver_ipv4() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {
            .use_custom_server = true,
            .address = "1.1.1.1",
            .port = 53,
        },
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = "test_driver",
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .ip_source = Config::IpSource::HTTP,
                    .ip_source_param = "https://api.ipify.org",
                },
            }},
        }},
    };
}

Config::AppConfig build_custom_resolver_invalid() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {
            .use_custom_server = true,
            .address = "999.999.999.999",
            .port = 53,
        },
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = "test_driver",
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .ip_source = Config::IpSource::HTTP,
                    .ip_source_param = "https://api.ipify.org",
                },
            }},
        }},
    };
}

Config::AppConfig build_interface_exists() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {},
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = "test_driver",
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .interface = "eth0",
                    .ip_type = AddressFamily::UNSPECIFIED,
                    .ip_source = Config::IpSource::INTERFACE,
                    .ip_source_param = "",
                }
            }},
        }},
    };
}

Config::AppConfig build_interface_not_found() {
    return Config::AppConfig{
        .driver = {},
        .resolver = {},
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 0,
            .driver = "test_driver",
            .subdomains = {{
                Config::SubdomainConfig{
                    .name = "www",
                    .type = RecordKind::A,
                    .interface = "eth0",
                    .ip_type = AddressFamily::UNSPECIFIED,
                    .ip_source = Config::IpSource::INTERFACE,
                    .ip_source_param = "",
                }
            }},
        }},
    };
}

} // anonymous namespace

// ── Helpers for building interface lists ────────────────────────────────────

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
} // anonymous namespace

// ── Parameterized test case definition ─────────────────────────────────────

struct ValidateCase {
    const char* name;
    std::vector<std::string_view> loaded_drivers;  // matches DriverManagerBase::get_loaded_drivers()
    std::vector<std::string> interfaces;           // matches ConfigValidator constructor
    int min_interval;
    bool should_throw;
    Config::AppConfig (*build)();
};

class ConfigValidatorValidateTest : public ::testing::TestWithParam<ValidateCase> {};

TEST_P(ConfigValidatorValidateTest, Validate) {
    const auto& param = GetParam();

    MockDriverManager mock;
    if (param.loaded_drivers.empty()) {
        EXPECT_CALL(mock, get_loaded_drivers())
            .WillOnce(testing::Return(std::vector<std::string_view>{}));
    } else if (param.should_throw) {
        EXPECT_CALL(mock, get_loaded_drivers())
            .WillOnce(testing::Return(param.loaded_drivers));
    } else {
        EXPECT_CALL(mock, get_loaded_drivers())
            .WillRepeatedly(testing::Return(param.loaded_drivers));
    }

    auto cfg = param.build();

    if (param.should_throw) {
        if (param.min_interval == 60) {
            Validator60 v(mock, param.interfaces);
            EXPECT_THROW(v.validate(cfg), ConfigVerificationException);
        } else {
            Validator300 v(mock, param.interfaces);
            EXPECT_THROW(v.validate(cfg), ConfigVerificationException);
        }
    } else {
        if (param.min_interval == 60) {
            Validator60 v(mock, param.interfaces);
            EXPECT_NO_THROW(v.validate(cfg));
        } else {
            Validator300 v(mock, param.interfaces);
            EXPECT_NO_THROW(v.validate(cfg));
        }
    }
}

// ── Test cases ─────────────────────────────────────────────────────────────

INSTANTIATE_TEST_SUITE_P(
    ,
    ConfigValidatorValidateTest,
    ::testing::Values(
        ValidateCase{"ValidConfig",                {"test_driver"},        {},  60, false, &build_default},
        ValidateCase{"MultipleDomains",            {"drv1", "drv2"},       {},  60, false, &build_multi_domain},
        ValidateCase{"DriverNotFound",             {"some_other_driver"},  {},  60, true,  &build_default},
        ValidateCase{"NoDriversLoaded",            {},                     {},  60, true,  &build_default},
        ValidateCase{"EmptyDomainName",            {"test_driver"},        {},  60, true,  &build_empty_domain_name},
        ValidateCase{"NoSubdomains",               {"test_driver"},        {},  60, true,  &build_no_subdomains},
        ValidateCase{"EmptySubdomainName",         {"test_driver"},        {},  60, true,  &build_empty_subdomain_name},
        ValidateCase{"UpdateIntervalBelowMinimum", {"test_driver"},        {}, 300, true,  &build_low_update_interval},
        ValidateCase{"UpdateIntervalAtMinimum",    {"test_driver"},        {},  60, false, &build_low_update_interval},
        ValidateCase{"SubdomainIntervalBelowMin",  {"test_driver"},        {},  60, true,  &build_subdomain_low_interval},
        ValidateCase{"SubdomainIntervalAtMin",     {"test_driver"},        {},  60, false, &build_subdomain_ok_interval},
        ValidateCase{"ForceUpdateLessThanUpdate",  {"test_driver"},        {},  60, true,  &build_force_update_less_than_update},
        ValidateCase{"ForceUpdateDisabled",        {"test_driver"},        {},  60, false, &build_force_update_disabled},
        ValidateCase{"CustomResolverDoH",          {"test_driver"},        {},  60, false, &build_custom_resolver_doh},
        ValidateCase{"CustomResolverIPv4",         {"test_driver"},        {},  60, false, &build_custom_resolver_ipv4},
        ValidateCase{"CustomResolverInvalid",      {"test_driver"},        {},  60, true,  &build_custom_resolver_invalid},
        ValidateCase{"InterfaceExists",            {"test_driver"},  ifaces({"eth0"}),                  60, false, &build_interface_exists},
        ValidateCase{"InterfaceNotFound",          {"test_driver"},  ifaces({"eth1"}),                  60, true,  &build_interface_not_found}
    ),
    [](const ::testing::TestParamInfo<ValidateCase>& info) {
        return std::string(info.param.name);
    }
);
