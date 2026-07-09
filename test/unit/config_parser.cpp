//
// Unit tests for config/parser.hpp — glaze-based JSON config parsing.
//
// Verifies:
//   - Minimal config parses successfully with default values.
//   - Full config with all fields parses correctly.
//   - Backward-compatible key names ("ipaddress", "url") work.
//   - All SubdomainConfig fields round-trip correctly.
//   - Invalid JSON is rejected.
//   - Wrong type values produce errors.
//   - Empty domain list is valid.
//   - mDNS-specific config parses.
// =============================================================================

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <glaze/glaze.hpp>

#include "config/config.h"
#include "config/parser.hpp"
#include "fixtures/sample_config.h"

// ===========================================================================
// Config::AppConfig parsing helpers
// ===========================================================================

/// Parse a JSON string into a Config::AppConfig.
/// Returns the parsed struct on success, or a glaze error context on failure.
struct ParseResult {
    Config::AppConfig value{};
    glz::error_ctx ec{};
    bool ok{false};
};

[[nodiscard]] static ParseResult parse_config(std::string_view json) {
    ParseResult result{};
    const std::string s(json);
    result.ec = glz::read_json(result.value, s);
    result.ok = !result.ec;
    return result;
}

// ===========================================================================
// Minimal config
// ===========================================================================

TEST(ConfigParserTest, MinimalConfig_ParsesSuccessfully) {
    auto result = parse_config(Fixtures::MINIMAL_CONFIG);
    ASSERT_TRUE(result.ok);

    const auto& cfg = result.value;
    EXPECT_TRUE(cfg.driver.auto_discover);
    EXPECT_FALSE(cfg.driver.driver_dir.has_value());
    EXPECT_TRUE(cfg.driver.load.empty());
    EXPECT_FALSE(cfg.resolver.use_custom_server);
    EXPECT_TRUE(cfg.resolver.servers.empty());
    EXPECT_EQ(cfg.resolver.port, 53);
    EXPECT_TRUE(cfg.domains.empty());
}

// ===========================================================================
// Full config
// ===========================================================================

TEST(ConfigParserTest, FullConfig_ParsesAllFields) {
    auto result = parse_config(Fixtures::FULL_CONFIG);
    ASSERT_TRUE(result.ok);

    const auto& cfg = result.value;

    // Driver config
    ASSERT_TRUE(cfg.driver.driver_dir.has_value());
    EXPECT_EQ(*cfg.driver.driver_dir, "/usr/lib/yaddnsc/drivers");
    EXPECT_TRUE(cfg.driver.auto_discover);
    ASSERT_EQ(cfg.driver.load.size(), 2U);
    EXPECT_EQ(cfg.driver.load[0], "cloudflare");
    EXPECT_EQ(cfg.driver.load[1], "digital_ocean");

    // Resolver config
    EXPECT_TRUE(cfg.resolver.use_custom_server);
    ASSERT_EQ(cfg.resolver.servers.size(), 2U);
    EXPECT_EQ(cfg.resolver.servers[0].address, "1.1.1.1");
    EXPECT_EQ(cfg.resolver.servers[0].port, 53);
    EXPECT_EQ(cfg.resolver.servers[1].address, "8.8.8.8");
    EXPECT_EQ(cfg.resolver.servers[1].port, 53);
    EXPECT_EQ(cfg.resolver.strategy, Config::ResolverStrategy::FALLBACK);

    // Domains
    ASSERT_EQ(cfg.domains.size(), 1U);
    const auto& domain = cfg.domains[0];
    EXPECT_EQ(domain.name, "example.com");
    EXPECT_EQ(domain.update_interval, 300);
    EXPECT_EQ(domain.force_update, 3600);
    EXPECT_EQ(domain.driver, "cloudflare");

    // Subdomains
    ASSERT_EQ(domain.subdomains.size(), 2U);

    EXPECT_EQ(domain.subdomains[0].name, "@");
    EXPECT_EQ(domain.subdomains[0].type, RecordKind::A);
    EXPECT_EQ(domain.subdomains[0].ip_source, Config::IpSource::HTTP);
    EXPECT_EQ(domain.subdomains[0].ip_source_param, "https://api.ipify.org");

    EXPECT_EQ(domain.subdomains[1].name, "www");
    EXPECT_EQ(domain.subdomains[1].type, RecordKind::AAAA);
    EXPECT_EQ(domain.subdomains[1].ip_source, Config::IpSource::INTERFACE);
    EXPECT_EQ(domain.subdomains[1].interface, "eth0");
    EXPECT_EQ(domain.subdomains[1].ip_type, AddressFamily::IPV6);
}

// ===========================================================================
// Backward-compatible keys
// ===========================================================================

TEST(ConfigParserTest, BackwardCompat_Keys_AreAccepted) {
    auto result = parse_config(Fixtures::BACKWARD_COMPAT_CONFIG);
    ASSERT_TRUE(result.ok);

    const auto& cfg = result.value;

    // "ipaddress" alias for resolver address
    EXPECT_TRUE(cfg.resolver.use_custom_server);
    EXPECT_EQ(cfg.resolver.address, "9.9.9.9");
    EXPECT_EQ(cfg.resolver.port, 53);
    EXPECT_EQ(cfg.resolver.strategy, Config::ResolverStrategy::CONCURRENT);

    // "url" alias for IP source = HTTP
    ASSERT_EQ(cfg.domains.size(), 1U);
    ASSERT_EQ(cfg.domains[0].subdomains.size(), 1U);
    EXPECT_EQ(cfg.domains[0].subdomains[0].ip_source, Config::IpSource::HTTP);
    EXPECT_EQ(cfg.domains[0].subdomains[0].ip_source_param, "https://api6.ipify.org");
}

// ===========================================================================
// All subdomain fields
// ===========================================================================

TEST(ConfigParserTest, AllSubdomainFields_ParseCorrectly) {
    auto result = parse_config(Fixtures::ALL_SUBDOMAIN_FIELDS);
    ASSERT_TRUE(result.ok);

    const auto& cfg = result.value;
    ASSERT_EQ(cfg.domains.size(), 1U);
    const auto& domain = cfg.domains[0];

    EXPECT_EQ(domain.name, "test.net");
    EXPECT_EQ(domain.update_interval, 300);
    EXPECT_EQ(domain.force_update, 1800);
    EXPECT_EQ(domain.driver, "cloudflare");

    ASSERT_EQ(domain.subdomains.size(), 1U);
    const auto& sub = domain.subdomains[0];

    EXPECT_EQ(sub.name, "api");
    EXPECT_EQ(sub.type, RecordKind::TXT);
    EXPECT_EQ(sub.interface, "bond0");
    EXPECT_EQ(sub.ip_type, AddressFamily::UNSPECIFIED);
    EXPECT_EQ(sub.ip_source, Config::IpSource::HTTP);
    EXPECT_EQ(sub.ip_source_param, "https://checkip.amazonaws.com");
    EXPECT_TRUE(sub.allow_ula);
    EXPECT_FALSE(sub.allow_local_link);
    EXPECT_EQ(sub.update_interval, 60);
}

// ===========================================================================
// mDNS config
// ===========================================================================

TEST(ConfigParserTest, MdnsConfig_ParsesSuccessfully) {
    auto result = parse_config(Fixtures::MDNS_CONFIG);
    ASSERT_TRUE(result.ok);

    const auto& cfg = result.value;
    ASSERT_EQ(cfg.domains.size(), 1U);
    ASSERT_EQ(cfg.domains[0].subdomains.size(), 1U);

    const auto& sub = cfg.domains[0].subdomains[0];
    EXPECT_EQ(sub.name, "printer");
    EXPECT_EQ(sub.type, RecordKind::A);
    EXPECT_EQ(sub.ip_source, Config::IpSource::MDNS);
    EXPECT_EQ(sub.ip_source_param, "printer.local");
}

// ===========================================================================
// Empty domain list
// ===========================================================================

TEST(ConfigParserTest, EmptyDomains_ParsesSuccessfully) {
    auto result = parse_config(Fixtures::EMPTY_DOMAINS_CONFIG);
    ASSERT_TRUE(result.ok);

    const auto& cfg = result.value;
    EXPECT_FALSE(cfg.driver.auto_discover);
    ASSERT_TRUE(cfg.driver.driver_dir.has_value());
    EXPECT_EQ(*cfg.driver.driver_dir, "./drivers");
    EXPECT_TRUE(cfg.driver.load.empty());
    EXPECT_TRUE(cfg.domains.empty());
}

// ===========================================================================
// Error paths
// ===========================================================================

TEST(ConfigParserTest, InvalidJson_ReturnsError) {
    auto result = parse_config(Fixtures::INVALID_JSON);
    ASSERT_FALSE(result.ok);
}

TEST(ConfigParserTest, WrongType_ReturnsError) {
    // "auto_discover": "not_a_boolean" should fail type validation.
    auto result = parse_config(Fixtures::WRONG_TYPE_VALUE);
    ASSERT_FALSE(result.ok);
}
