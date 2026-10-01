//
// Unit tests for config/parser.hpp — glaze-based JSON config parsing.
//
// Verifies:
//   - Minimal config parses successfully with default values.
//   - Full config with all fields parses correctly.
//   - Backward-compatible IP source name ("url") works.
//   - All SubdomainConfig fields round-trip correctly.
//   - Invalid JSON is rejected.
//   - Wrong type values produce errors.
//   - Empty domain list is valid.
//   - mDNS-specific config parses.
// =============================================================================

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <gtest/gtest.h>

#include "domain/config/dns_config.h"
#include "domain/config/ip_source_kind.h"
#include "domain/dns/record_kind.h"
#include "domain/network/address_family.h"
#include "fixtures/sample_config.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/parser.hpp"  // IWYU pragma: keep — registers glz::meta specializations

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

TEST(ConfigParserTest, PluralConfigurationKeys_ParseAndSerializeWithDomainDriverUnchanged) {
    const auto parsed = parse_config(R"({
        "drivers": {"auto_discover":true},
        "resolver": {"use_custom_servers":true,"servers":[{"address":"1.1.1.1"}]},
        "domains": [{"driver":"simple","subdomains":[{"driver_params":{"url":"https://example.com"}}]}]
    })");
    ASSERT_TRUE(parsed.ok);
    EXPECT_TRUE(parsed.value.drivers.auto_discover);
    EXPECT_TRUE(parsed.value.resolver.use_custom_servers);
    ASSERT_EQ(parsed.value.domains.size(), 1U);
    EXPECT_EQ(parsed.value.domains.front().driver, "simple");
    const auto written = glz::write_json(parsed.value);
    ASSERT_TRUE(written.has_value());
    EXPECT_NE(written->find("\"drivers\":"), std::string::npos);
    EXPECT_NE(written->find("\"use_custom_servers\":"), std::string::npos);
    EXPECT_NE(written->find("\"driver_params\":"), std::string::npos);
    EXPECT_NE(written->find("\"driver\":\"simple\""), std::string::npos);
}

TEST(ConfigParserTest, SingularConfigurationKeys_ReturnUnknownKey) {
    EXPECT_EQ(parse_config(R"({"driver":{}})").ec.ec, glz::error_code::unknown_key);
    EXPECT_EQ(parse_config(R"({"resolver":{"use_custom_server":true}})").ec.ec, glz::error_code::unknown_key);
    EXPECT_EQ(parse_config(R"({"domains":[{"subdomains":[{"driver_param":{}}]}]})").ec.ec,
              glz::error_code::unknown_key);
}

TEST(ConfigParserTest, SubdomainConfig_RemovedIpType_IsRejected) {
    Config::SubdomainConfig config;
    const auto error = glz::read_json(config, std::string{R"({"ip_type":"ipv4"})"});
    EXPECT_EQ(error.ec, glz::error_code::unknown_key);
}

TEST(ConfigParserTest, SubdomainConfig_Serialization_DoesNotIncludeIpType) {
    const auto written = glz::write_json(Config::SubdomainConfig{});
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written->find("ip_type"), std::string::npos);
}

// ===========================================================================
// Minimal config
// ===========================================================================

TEST(ConfigParserTest, MinimalConfig_ParsesSuccessfully) {
    auto result = parse_config(Fixtures::MINIMAL_CONFIG);
    ASSERT_TRUE(result.ok);

    const auto& cfg = result.value;
    EXPECT_TRUE(cfg.drivers.auto_discover);
    EXPECT_FALSE(cfg.drivers.driver_dir.has_value());
    EXPECT_TRUE(cfg.drivers.load.empty());
    EXPECT_FALSE(cfg.resolver.use_custom_servers);
    EXPECT_TRUE(cfg.resolver.servers.empty());
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
    ASSERT_TRUE(cfg.drivers.driver_dir.has_value());
    EXPECT_EQ(*cfg.drivers.driver_dir, "/usr/lib/yaddnsc/drivers");
    EXPECT_TRUE(cfg.drivers.auto_discover);
    ASSERT_EQ(cfg.drivers.load.size(), 2U);
    EXPECT_EQ(cfg.drivers.load[0], "cloudflare");
    EXPECT_EQ(cfg.drivers.load[1], "digital_ocean");

    // Resolver config
    EXPECT_TRUE(cfg.resolver.use_custom_servers);
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
}

// ===========================================================================
// Backward-compatible keys
// ===========================================================================

TEST(ConfigParserTest, ShuffleStrategy_ParsesSuccessfully) {
    constexpr std::string_view json = R"({
        "drivers": { "auto_discover": true },
        "resolver": { "use_custom_servers": true, "strategy": "shuffle",
                      "servers": [{"address": "1.1.1.1"}] },
        "domains": []
    })";
    auto result = parse_config(json);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.value.resolver.strategy, Config::ResolverStrategy::SHUFFLE);
}

TEST(ConfigParserTest, ResolverConfig_DirectServerFields_ReturnUnknownKey) {
    for (const auto& json : {R"({"address":"1.1.1.1"})", R"({"ipaddress":"1.1.1.1"})", R"({"port":53})"}) {
        Config::ResolverConfig config;
        const auto error = glz::read_json(config, std::string(json));
        EXPECT_EQ(error.ec, glz::error_code::unknown_key) << json;
    }
}

TEST(ConfigParserTest, ResolverConfig_Serialization_ContainsOnlyListSettings) {
    const auto written = glz::write_json(Config::ResolverConfig{});
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(*written, R"({"use_custom_servers":false,"servers":[],"strategy":"concurrent"})");
}

TEST(ConfigParserTest, BackwardCompat_Keys_AreAccepted) {
    auto result = parse_config(Fixtures::BACKWARD_COMPAT_CONFIG);
    ASSERT_TRUE(result.ok);

    const auto& cfg = result.value;

    EXPECT_TRUE(cfg.resolver.use_custom_servers);
    ASSERT_EQ(cfg.resolver.servers.size(), 1U);
    EXPECT_EQ(cfg.resolver.servers[0].address, "9.9.9.9");
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
    EXPECT_FALSE(cfg.drivers.auto_discover);
    ASSERT_TRUE(cfg.drivers.driver_dir.has_value());
    EXPECT_EQ(*cfg.drivers.driver_dir, "./drivers");
    EXPECT_TRUE(cfg.drivers.load.empty());
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
