//
// Unit tests for config/static_validator.h — validate_static +
// validate_and_normalize, plus domain::make_fqdn (used by the validator for
// its messages; moved from Config to the domain layer in Phase 3).
//
// Verified:
//   - make_fqdn — correct FQDN construction (apex / empty / deep labels).
//   - validate_static — every static check reports a ConfigError value with
//     the exact legacy ConfigValidator message; all violations are collected
//     in visitation order instead of failing fast.
//   - validate_and_normalize — success returns the normalised runtime config,
//     failure returns the collected errors.
// =============================================================================

#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "config/static_validator.h"
#include "domain/fqdn.h"
#include "fmt.hpp"
#include "min_update_interval.h"

using testing::ElementsAre;
using testing::HasSubstr;

namespace {

using Code = domain::ConfigError::Code;

/// Build a minimal raw AppConfig with one domain and one subdomain.
[[nodiscard]] Config::AppConfig make_domain_config(
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

/// Run validate_static over a config built by mutating the minimal default.
template<typename Mutator>
[[nodiscard]] std::vector<domain::ConfigError> validate_with(Mutator mut) {
    auto cfg = make_domain_config();
    mut(cfg);
    return Config::validate_static(cfg);
}

} // anonymous namespace

// ===========================================================================
// domain::make_fqdn
// ===========================================================================

TEST(MakeFqdnTest, SubdomainAndDomain) {
    EXPECT_EQ(domain::make_fqdn("example.com", "www"), "www.example.com");
}

TEST(MakeFqdnTest, ApexSubdomain) {
    EXPECT_EQ(domain::make_fqdn("example.com", "@"), "example.com");
}

TEST(MakeFqdnTest, EmptySubdomain) {
    EXPECT_EQ(domain::make_fqdn("example.com", ""), "example.com");
}

TEST(MakeFqdnTest, DeepSubdomain) {
    EXPECT_EQ(domain::make_fqdn("example.com", "a.b.c"), "a.b.c.example.com");
}

// ===========================================================================
// validate_static — domain-level checks
// ===========================================================================

TEST(StaticValidatorTest, ValidConfig_NoErrors) {
    EXPECT_TRUE(Config::validate_static(make_domain_config()).empty());
}

TEST(StaticValidatorTest, EmptyDomainName) {
    const auto errors = validate_with([](Config::AppConfig &cfg) { cfg.domains[0].name = ""; });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::EMPTY_DOMAIN_NAME);
    EXPECT_EQ(errors[0].message, "Domain name must not be empty");
}

TEST(StaticValidatorTest, NoSubdomains) {
    const auto errors = validate_with([](Config::AppConfig &cfg) { cfg.domains[0].subdomains.clear(); });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::EMPTY_SUBDOMAINS);
    EXPECT_EQ(errors[0].message, "Domain 'example.com' must have at least one subdomain");
}

TEST(StaticValidatorTest, UpdateIntervalBelowMinimum) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.domains[0].update_interval = YADDNSC_MIN_UPDATE_INTERVAL - 1;
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::UPDATE_INTERVAL_LOW);
    EXPECT_EQ(errors[0].message,
              fmt::format("Update interval too low for domain example.com ({}), minimal interval: {}",
                          YADDNSC_MIN_UPDATE_INTERVAL - 1, YADDNSC_MIN_UPDATE_INTERVAL));
}

TEST(StaticValidatorTest, UpdateIntervalAtMinimum_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.domains[0].update_interval = YADDNSC_MIN_UPDATE_INTERVAL;
    });
    EXPECT_TRUE(errors.empty());
}

TEST(StaticValidatorTest, ForceUpdateSmallerThanInterval) {
    const auto errors = validate_with([](Config::AppConfig &cfg) { cfg.domains[0].force_update = 30; });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::FORCE_UPDATE_CONFLICT);
    EXPECT_EQ(errors[0].message,
              "Force update interval for domain example.com must not be smaller than the update interval (300)");
}

TEST(StaticValidatorTest, ForceUpdateDisabled_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) { cfg.domains[0].force_update = 0; });
    EXPECT_TRUE(errors.empty());
}

TEST(StaticValidatorTest, ForceUpdateGreaterThanInterval_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) { cfg.domains[0].force_update = 600; });
    EXPECT_TRUE(errors.empty());
}

// ===========================================================================
// validate_static — subdomain-level checks
// ===========================================================================

TEST(StaticValidatorTest, EmptySubdomainName) {
    const auto errors = validate_with([](Config::AppConfig &cfg) { cfg.domains[0].subdomains[0].name = ""; });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::EMPTY_SUBDOMAIN_NAME);
    EXPECT_EQ(errors[0].message, "Subdomain name must not be empty in domain 'example.com'");
}

TEST(StaticValidatorTest, SubdomainIntervalBelowMinimum) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.domains[0].subdomains[0].update_interval = YADDNSC_MIN_UPDATE_INTERVAL - 1;
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::UPDATE_INTERVAL_LOW);
    EXPECT_EQ(errors[0].message,
              fmt::format("Update interval too low for subdomain www.example.com ({}), minimal interval: {}",
                          YADDNSC_MIN_UPDATE_INTERVAL - 1, YADDNSC_MIN_UPDATE_INTERVAL));
}

TEST(StaticValidatorTest, SubdomainIntervalAtMinimum_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.domains[0].subdomains[0].update_interval = YADDNSC_MIN_UPDATE_INTERVAL;
    });
    EXPECT_TRUE(errors.empty());
}

// ===========================================================================
// validate_static — IP source checks
// ===========================================================================

TEST(StaticValidatorTest, InterfaceSource_WithInterface_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        auto &sub = cfg.domains[0].subdomains[0];
        sub.ip_source = Config::IpSource::INTERFACE;
        sub.interface = "eth0";
        sub.ip_source_param = "";
    });
    EXPECT_TRUE(errors.empty());
}

TEST(StaticValidatorTest, InterfaceSource_EmptyInterface) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        auto &sub = cfg.domains[0].subdomains[0];
        sub.ip_source = Config::IpSource::INTERFACE;
        sub.interface = "";
        sub.ip_source_param = "";
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::MISSING_INTERFACE);
    EXPECT_EQ(errors[0].message, "Subdomain www.example.com uses interface IP source but 'interface' field is empty");
}

TEST(StaticValidatorTest, HttpSource_EmptyParam) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.domains[0].subdomains[0].ip_source_param = "";
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::EMPTY_IP_SOURCE_PARAM);
    EXPECT_EQ(errors[0].message, "Subdomain www.example.com uses HTTP IP source but ip_source_param is empty");
}

TEST(StaticValidatorTest, HttpSource_InvalidUrl) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.domains[0].subdomains[0].ip_source_param = "not-a-url";
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::INVALID_IP_SOURCE_URL);
    EXPECT_THAT(errors[0].message,
                HasSubstr("Subdomain www.example.com has invalid ip_source_param 'not-a-url': "));
}

TEST(StaticValidatorTest, HttpSource_MissingHost) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.domains[0].subdomains[0].ip_source_param = "http://:8080/path";
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::INVALID_IP_SOURCE_URL);
}

TEST(StaticValidatorTest, MdnsSource_ValidLocalDomain_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        auto &sub = cfg.domains[0].subdomains[0];
        sub.ip_source = Config::IpSource::MDNS;
        sub.ip_source_param = "printer.local";
    });
    EXPECT_TRUE(errors.empty());
}

TEST(StaticValidatorTest, MdnsSource_TrailingDot_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        auto &sub = cfg.domains[0].subdomains[0];
        sub.type = RecordKind::AAAA;
        sub.ip_source = Config::IpSource::MDNS;
        sub.ip_source_param = "printer.local.";
    });
    EXPECT_TRUE(errors.empty());
}

TEST(StaticValidatorTest, MdnsSource_EmptyParam) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        auto &sub = cfg.domains[0].subdomains[0];
        sub.ip_source = Config::IpSource::MDNS;
        sub.ip_source_param = "";
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::EMPTY_IP_SOURCE_PARAM);
    EXPECT_EQ(errors[0].message, "Subdomain www.example.com uses mDNS IP source but ip_source_param is empty");
}

TEST(StaticValidatorTest, MdnsSource_InvalidDomain) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        auto &sub = cfg.domains[0].subdomains[0];
        sub.ip_source = Config::IpSource::MDNS;
        sub.ip_source_param = "not valid .local";
    });
    ASSERT_GE(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::INVALID_MDNS_NAME);
    EXPECT_EQ(errors[0].message,
              "Subdomain www.example.com has invalid domain name 'not valid .local' for mDNS IP source");
}

TEST(StaticValidatorTest, MdnsSource_NonLocalSuffix) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        auto &sub = cfg.domains[0].subdomains[0];
        sub.ip_source = Config::IpSource::MDNS;
        sub.ip_source_param = "printer.example.com";
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::MDNS_NOT_LOCAL);
    EXPECT_EQ(errors[0].message,
              "Subdomain www.example.com uses mDNS IP source but domain 'printer.example.com' does not end with "
              "'.local' (RFC 6762)");
}

TEST(StaticValidatorTest, MdnsSource_TxtType) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        auto &sub = cfg.domains[0].subdomains[0];
        sub.type = RecordKind::TXT;
        sub.ip_source = Config::IpSource::MDNS;
        sub.ip_source_param = "printer.local";
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::MDNS_BAD_RECORD_TYPE);
    EXPECT_EQ(errors[0].message, "Subdomain www.example.com uses mDNS IP source but type must be 'a' or 'aaaa'");
}

// ===========================================================================
// validate_static — resolver checks
// ===========================================================================

TEST(StaticValidatorTest, ResolverDoH_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.resolver.use_custom_server = true;
        cfg.resolver.servers = {{.address = "https://dns.cloudflare.com/dns-query", .port = 443}};
    });
    EXPECT_TRUE(errors.empty());
}

TEST(StaticValidatorTest, ResolverDoT_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.resolver.use_custom_server = true;
        cfg.resolver.servers = {{.address = "tls://1.1.1.1:853", .port = 853}};
    });
    EXPECT_TRUE(errors.empty());
}

TEST(StaticValidatorTest, ResolverLegacyIPv4_NoErrors) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.resolver.use_custom_server = true;
        cfg.resolver.address = "1.1.1.1";
        cfg.resolver.port = 53;
    });
    EXPECT_TRUE(errors.empty());
}

TEST(StaticValidatorTest, ResolverInvalidIPv4) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.resolver.use_custom_server = true;
        cfg.resolver.address = "999.999.999.999";
        cfg.resolver.port = 53;
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::INVALID_RESOLVER);
    EXPECT_EQ(errors[0].message, "Invalid resolver address 999.999.999.999");
}

TEST(StaticValidatorTest, ResolverHostnameAddress) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.resolver.use_custom_server = true;
        cfg.resolver.address = "resolver.example.com";
        cfg.resolver.port = 53;
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::INVALID_RESOLVER);
    EXPECT_EQ(errors[0].message, "Invalid resolver address resolver.example.com");
}

TEST(StaticValidatorTest, ResolverDoHEmptyHost) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.resolver.use_custom_server = true;
        cfg.resolver.servers = {{.address = "https:///dns-query", .port = 443}};
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::INVALID_RESOLVER);
    EXPECT_EQ(errors[0].message, R"(DoH/DoT resolver address "https:///dns-query" has an empty host)");
}

TEST(StaticValidatorTest, ResolverDoTPortZero) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.resolver.use_custom_server = true;
        cfg.resolver.servers = {{.address = "tls://1.1.1.1:0", .port = 853}};
    });
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::INVALID_RESOLVER);
    EXPECT_EQ(errors[0].message, R"(DoH/DoT resolver address "tls://1.1.1.1:0" has port 0)");
}

TEST(StaticValidatorTest, ResolverNotCustom_NotChecked) {
    const auto errors = validate_with([](Config::AppConfig &cfg) {
        cfg.resolver.use_custom_server = false;
        cfg.resolver.address = "999.999.999.999"; // ignored: no custom server
    });
    EXPECT_TRUE(errors.empty());
}

// ===========================================================================
// validate_static — collect-all semantics
// ===========================================================================

TEST(StaticValidatorTest, CollectsAllErrorsInVisitationOrder) {
    auto cfg = make_domain_config("", 10); // empty name + low interval
    cfg.domains[0].subdomains[0].name = ""; // empty subdomain name

    const auto errors = Config::validate_static(cfg);
    ASSERT_EQ(errors.size(), 3U);
    // Domain checks come before subdomain checks (legacy visitation order).
    EXPECT_EQ(errors[0].code, Code::EMPTY_DOMAIN_NAME);
    EXPECT_EQ(errors[1].code, Code::UPDATE_INTERVAL_LOW);
    EXPECT_EQ(errors[2].code, Code::EMPTY_SUBDOMAIN_NAME);
}

TEST(StaticValidatorTest, MultiDomain_CollectsAcrossDomains) {
    auto cfg = make_domain_config("a.com", 300);
    cfg.domains.push_back(Config::DomainConfig{
        .name = "b.com",
        .update_interval = 1,
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
    });

    const auto errors = Config::validate_static(cfg);
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, Code::UPDATE_INTERVAL_LOW);
    EXPECT_THAT(errors[0].message, HasSubstr("b.com"));
}

// ===========================================================================
// validate_and_normalize
// ===========================================================================

TEST(StaticValidatorTest, ValidateAndNormalize_Success) {
    const auto result = Config::validate_and_normalize(make_domain_config());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->domains.size(), 1U);
    EXPECT_EQ(result->domains[0].name, "example.com");
    ASSERT_EQ(result->domains[0].subdomains.size(), 1U);
    // Effective interval: no subdomain override → domain value.
    EXPECT_EQ(result->domains[0].subdomains[0].update_interval, 300);
}

TEST(StaticValidatorTest, ValidateAndNormalize_FailureReturnsAllErrors) {
    auto cfg = make_domain_config("", 10);
    const auto result = Config::validate_and_normalize(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().size(), 2U);
    EXPECT_EQ(result.error().front().code, Code::EMPTY_DOMAIN_NAME);
}
