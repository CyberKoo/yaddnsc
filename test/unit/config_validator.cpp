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
// =============================================================================

#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "config/validator.hpp"
#include "mocks/mock_driver_manager.h"

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
    // SubdomainConfig fields in declaration order:
    //   name, type, interface, ip_type, ip_source, ip_source_param,
    //   allow_ula, allow_local_link, update_interval, driver_param
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

TEST(ConfigValidatorDetailTest, ValidateIpSource_Mdns_TrailingDot_Accepted) {
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

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_DoH_Accepted) {
    EXPECT_NO_THROW(detail::validate_resolver_address("https://dns.cloudflare.com/dns-query"));
}

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_DoT_Accepted) {
    EXPECT_NO_THROW(detail::validate_resolver_address("tls://1.1.1.1:853"));
}

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_IPv4_Accepted) {
    EXPECT_NO_THROW(detail::validate_resolver_address("8.8.8.8"));
}

TEST(ConfigValidatorDetailTest, ValidateResolverAddress_IPv6_Accepted) {
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
    // Plain hostnames (without a URI scheme) are not valid resolver addresses.
    EXPECT_THROW(detail::validate_resolver_address("resolver.example.com"), ConfigVerificationException);
}

// ===========================================================================
// ConfigValidator::validate()  —  integration with MockDriverManager
// ===========================================================================

using Validator60 = ConfigValidator<60>;

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

                // ── Happy path ───────────────────────────────────────────────────────────────

TEST(ConfigValidatorValidateTest, ValidConfig_DoesNotThrow) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    ConfigValidator<60> validator(mock, {});
    auto cfg = make_domain_config();

    EXPECT_NO_THROW(validator.validate(cfg));
}

TEST(ConfigValidatorValidateTest, MultipleDomains_AllValid) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillRepeatedly(testing::Return(std::vector<std::string_view>{"drv1", "drv2"}));

    Config::AppConfig cfg{
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

    ConfigValidator<60> validator(mock, {});
    EXPECT_NO_THROW(validator.validate(cfg));
}

// ── Driver not loaded ─────────────────────────────────────────────────────────

TEST(ConfigValidatorValidateTest, DriverNotFound_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"some_other_driver"}));

    ConfigValidator<60> validator(mock, {});
    auto cfg = make_domain_config();

    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}

TEST(ConfigValidatorValidateTest, NoDriversLoaded_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{}));

    ConfigValidator<60> validator(mock, {});
    auto cfg = make_domain_config();

    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}

// ── Empty domain name ─────────────────────────────────────────────────────────

TEST(ConfigValidatorValidateTest, EmptyDomainName_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    ConfigValidator<60> validator(mock, {});
    auto cfg = make_domain_config("");

    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}

// ── Empty subdomains ──────────────────────────────────────────────────────────

TEST(ConfigValidatorValidateTest, NoSubdomains_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
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

    ConfigValidator<60> validator(mock, {});
    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}

// ── Empty subdomain name ──────────────────────────────────────────────────────

TEST(ConfigValidatorValidateTest, EmptySubdomainName_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    auto cfg = make_domain_config("example.com", 300, "test_driver", "");

    ConfigValidator<60> validator(mock, {});
    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}

// ── Update interval too low ───────────────────────────────────────────────────

TEST(ConfigValidatorValidateTest, UpdateIntervalBelowMinimum_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    // Use ConfigValidator<300> so interval 60 is below minimum.
    ConfigValidator<300> validator(mock, {});
    auto cfg = make_domain_config("example.com", 60);

    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}

TEST(ConfigValidatorValidateTest, UpdateIntervalAtMinimum_Ok) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    // Use ConfigValidator<60> so interval 60 is exactly the minimum.
    ConfigValidator<60> validator(mock, {});
    auto cfg = make_domain_config("example.com", 60);

    EXPECT_NO_THROW(validator.validate(cfg));
}

TEST(ConfigValidatorValidateTest, SubdomainUpdateIntervalBelowMinimum_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
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
                    .update_interval = 10,  // below minimum 60
                }
            }},
        }},
    };

    ConfigValidator<60> validator(mock, {});
    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}

TEST(ConfigValidatorValidateTest, SubdomainUpdateIntervalAtMinimum_Ok) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
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
                    .update_interval = 120,  // above minimum 60
                }
            }},
        }},
    };

    ConfigValidator<60> validator(mock, {});
    EXPECT_NO_THROW(validator.validate(cfg));
}

// ── Force-update interval ─────────────────────────────────────────────────────

TEST(ConfigValidatorValidateTest, ForceUpdateLessThanUpdate_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
        .driver = {},
        .resolver = {},
        .domains = {{
            .name = "example.com",
            .update_interval = 300,
            .force_update = 30,  // < 300
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

    ConfigValidator<60> validator(mock, {});
    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}

TEST(ConfigValidatorValidateTest, ForceUpdateDisabled_DoesNotThrow) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
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

    ConfigValidator<60> validator(mock, {});
    EXPECT_NO_THROW(validator.validate(cfg));
}

// ── Resolver address validation ───────────────────────────────────────────────

TEST(ConfigValidatorValidateTest, CustomResolver_ValidDoH_Ok) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillRepeatedly(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
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

    ConfigValidator<60> validator(mock, {});
    EXPECT_NO_THROW(validator.validate(cfg));
}

TEST(ConfigValidatorValidateTest, CustomResolver_ValidIPv4_Ok) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillRepeatedly(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
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

    ConfigValidator<60> validator(mock, {});
    EXPECT_NO_THROW(validator.validate(cfg));
}

TEST(ConfigValidatorValidateTest, CustomResolver_InvalidAddress_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillRepeatedly(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
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

    ConfigValidator<60> validator(mock, {});
    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}

// ── Interface existence check ─────────────────────────────────────────────────

TEST(ConfigValidatorValidateTest, ReferencedInterfaceExists_Ok) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
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

    ConfigValidator<60> validator(mock, {"eth0", "lo"});
    EXPECT_NO_THROW(validator.validate(cfg));
}

TEST(ConfigValidatorValidateTest, ReferencedInterfaceNotFound_Throws) {
    MockDriverManager mock;
    EXPECT_CALL(mock, get_loaded_drivers())
        .WillOnce(testing::Return(std::vector<std::string_view>{"test_driver"}));

    Config::AppConfig cfg{
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

    ConfigValidator<60> validator(mock, {"lo", "eth1"});  // eth0 not in list
    EXPECT_THROW(validator.validate(cfg), ConfigVerificationException);
}
