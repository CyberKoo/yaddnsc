//
// Unit tests for config/normalizer.h — raw AppConfig → domain::RuntimeConfig.
//
// Verified:
//   - Legacy resolver fields (use_custom_server + address/port) are folded
//     into the server list; disabled custom DNS materializes the default.
//   - SubdomainConfig::update_interval carries the EFFECTIVE value
//     (subdomain override if > 0, else the domain-level interval).
//   - driver_param is dumped to opaque JSON text preserving fields/values.
//   - normalize() never validates: statically-invalid configs still convert.
// =============================================================================

#include "infrastructure/config/normalizer.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <glaze/json/generic_fwd.hpp>
#include <gtest/gtest.h>

#include "domain/config/dns_config.h"
#include "domain/config/ip_source_kind.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/network/address_family.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/parser.hpp"  // IWYU pragma: keep — registers glz::meta specializations

namespace {

/// Parse raw JSON into an AppConfig (test helper; fails the test on error).
[[nodiscard]] Config::AppConfig parse_raw(std::string_view json) {
    Config::AppConfig cfg{};
    const auto ec = glz::read<glz::opts{.error_on_missing_keys = false}>(cfg, json);
    EXPECT_EQ(ec, glz::error_code::none) << glz::format_error(ec, json);
    return cfg;
}

/// Minimal valid raw config JSON with one domain/subdomain.
constexpr std::string_view MINIMAL_CONFIG = R"({
    "driver": { "auto_discover": false },
    "resolver": { "use_custom_server": false },
    "domains": [
        {
            "name": "example.com",
            "update_interval": 300,
            "driver": "test_driver",
            "subdomains": [
                {"name": "www", "type": "a", "ip_source": "http",
                 "ip_source_param": "https://api.ipify.org"}
            ]
        }
    ]
})";

}  // anonymous namespace

// ===========================================================================
// Resolver normalisation
// ===========================================================================

TEST(NormalizerTest, Resolver_NoCustomServer_MaterializesDefaultServer) {
    const auto config = Config::normalize(parse_raw(MINIMAL_CONFIG));
    ASSERT_EQ(config.resolver.servers.size(), 1U);
    EXPECT_EQ(config.resolver.servers.front().address, "1.1.1.1");
    EXPECT_EQ(config.resolver.servers.front().port, 53);
    EXPECT_EQ(config.resolver.strategy, Config::ResolverStrategy::CONCURRENT);
}

TEST(NormalizerTest, Resolver_CustomServers_CopiedThrough) {
    const auto raw = parse_raw(R"({
        "resolver": {
            "use_custom_server": true,
            "strategy": "fallback",
            "servers": [
                {"address": "1.1.1.1", "port": 53},
                {"address": "https://dns.cloudflare.com/dns-query", "port": 443}
            ]
        },
        "domains": []
    })");
    const auto config = Config::normalize(raw);
    ASSERT_EQ(config.resolver.servers.size(), 2U);
    EXPECT_EQ(config.resolver.servers[0].address, "1.1.1.1");
    EXPECT_EQ(config.resolver.servers[0].port, 53);
    EXPECT_EQ(config.resolver.servers[1].address, "https://dns.cloudflare.com/dns-query");
    EXPECT_EQ(config.resolver.strategy, Config::ResolverStrategy::FALLBACK);
}

TEST(NormalizerTest, Resolver_LegacySingleServer_FoldedIntoServers) {
    const auto raw = parse_raw(R"({
        "resolver": { "use_custom_server": true, "address": "8.8.8.8", "port": 5353 },
        "domains": []
    })");
    const auto config = Config::normalize(raw);
    ASSERT_EQ(config.resolver.servers.size(), 1U);
    EXPECT_EQ(config.resolver.servers[0].address, "8.8.8.8");
    EXPECT_EQ(config.resolver.servers[0].port, 5353);
}

TEST(NormalizerTest, Resolver_ServersTakePrecedenceOverLegacyAddress) {
    const auto raw = parse_raw(R"({
        "resolver": {
            "use_custom_server": true,
            "address": "8.8.8.8",
            "port": 5353,
            "servers": [{"address": "1.1.1.1", "port": 53}]
        },
        "domains": []
    })");
    const auto config = Config::normalize(raw);
    ASSERT_EQ(config.resolver.servers.size(), 1U);
    EXPECT_EQ(config.resolver.servers[0].address, "1.1.1.1");
}

TEST(NormalizerTest, Resolver_CustomServerWithoutAnyAddress_StaysEmpty) {
    const auto raw = parse_raw(R"({
        "resolver": { "use_custom_server": true },
        "domains": []
    })");
    const auto config = Config::normalize(raw);
    EXPECT_TRUE(config.resolver.servers.empty());
}

TEST(NormalizerTest, Resolver_LegacyAddressIgnoredWhenNotCustom) {
    const auto raw = parse_raw(R"({
        "resolver": { "use_custom_server": false, "address": "8.8.8.8", "port": 5353 },
        "domains": []
    })");
    const auto config = Config::normalize(raw);
    ASSERT_EQ(config.resolver.servers.size(), 1U);
    EXPECT_EQ(config.resolver.servers.front().address, "1.1.1.1");
}

TEST(NormalizerTest, Resolver_ShuffleStrategy_Preserved) {
    const auto raw = parse_raw(R"({
        "resolver": { "use_custom_server": true, "strategy": "shuffle",
                      "servers": [{"address": "1.1.1.1", "port": 53}] },
        "domains": []
    })");
    const auto config = Config::normalize(raw);
    EXPECT_EQ(config.resolver.strategy, Config::ResolverStrategy::SHUFFLE);
    ASSERT_EQ(config.resolver.servers.size(), 1U);
}

// ===========================================================================
// Driver settings normalisation
// ===========================================================================

TEST(NormalizerTest, DriverDir_Unset_StaysNullopt) {
    const auto config = Config::normalize(parse_raw(MINIMAL_CONFIG));
    EXPECT_FALSE(config.driver.driver_dir.has_value());
    EXPECT_FALSE(config.driver.auto_discover);
    EXPECT_TRUE(config.driver.load.empty());
}

TEST(NormalizerTest, DriverDir_Set_ConvertedToPath) {
    const auto raw = parse_raw(R"({
        "driver": { "driver_dir": "/opt/drivers", "auto_discover": true, "load": ["a.so", "b.so"] },
        "domains": []
    })");
    const auto config = Config::normalize(raw);
    ASSERT_TRUE(config.driver.driver_dir.has_value());
    EXPECT_EQ(*config.driver.driver_dir, std::filesystem::path("/opt/drivers"));
    EXPECT_TRUE(config.driver.auto_discover);
    EXPECT_EQ(config.driver.load, (std::vector<std::string>{"a.so", "b.so"}));
}

TEST(NormalizerTest, DriverDir_EmptyString_PreservedAsEmptyPath) {
    // "set but empty" must survive normalisation: DriverLoader rejects it.
    const auto raw = parse_raw(R"({
        "driver": { "driver_dir": "", "load": ["a.so"] },
        "domains": []
    })");
    const auto config = Config::normalize(raw);
    ASSERT_TRUE(config.driver.driver_dir.has_value());
    EXPECT_TRUE(config.driver.driver_dir->empty());
}

// ===========================================================================
// Effective update interval
// ===========================================================================

TEST(NormalizerTest, SubdomainInterval_InheritsDomainValue) {
    const auto config = Config::normalize(parse_raw(MINIMAL_CONFIG));
    EXPECT_EQ(config.domains[0].subdomains[0].update_interval, 300);
}

TEST(NormalizerTest, SubdomainInterval_OverrideWins) {
    auto raw = parse_raw(MINIMAL_CONFIG);
    raw.domains[0].subdomains[0].update_interval = 120;
    const auto config = Config::normalize(raw);
    EXPECT_EQ(config.domains[0].subdomains[0].update_interval, 120);
    // Domain-level value is untouched.
    EXPECT_EQ(config.domains[0].update_interval, 300);
}

TEST(NormalizerTest, SubdomainInterval_ZeroMeansInherit) {
    auto raw = parse_raw(MINIMAL_CONFIG);
    raw.domains[0].subdomains[0].update_interval = 0;
    const auto config = Config::normalize(raw);
    EXPECT_EQ(config.domains[0].subdomains[0].update_interval, 300);
}

TEST(NormalizerTest, SubdomainType_Missing_FallsBackToA) {
    const auto raw = parse_raw(R"({
        "domains": [{
            "name": "example.com",
            "update_interval": 300,
            "driver": "test_driver",
            "subdomains": [
                {"name": "www", "ip_source": "http", "ip_source_param": "https://api.ipify.org"}
            ]
        }]
    })");
    const auto config = Config::normalize(raw);
    // Legacy configs without "type" keep running as A records (warn logged).
    EXPECT_EQ(config.domains[0].subdomains[0].type, RecordKind::A);
}

// ===========================================================================
// driver_param normalisation
// ===========================================================================

TEST(NormalizerTest, DriverParam_Unset_BecomesEmptyObject) {
    const auto config = Config::normalize(parse_raw(MINIMAL_CONFIG));
    // An unset driver_param arrives as glz::generic null; the driver must
    // receive "{}" — never the literal "null".
    EXPECT_EQ(config.domains[0].subdomains[0].driver_param, "{}");
}

TEST(NormalizerTest, DriverParam_ExplicitNull_BecomesEmptyObject) {
    const auto raw = parse_raw(R"({
        "domains": [{
            "name": "example.com",
            "update_interval": 300,
            "driver": "test_driver",
            "subdomains": [{
                "name": "www", "type": "a", "ip_source": "http",
                "ip_source_param": "https://api.ipify.org",
                "driver_param": null
            }]
        }]
    })");
    const auto config = Config::normalize(raw);
    EXPECT_EQ(config.domains[0].subdomains[0].driver_param, "{}");
}

TEST(NormalizerTest, DriverParam_PreservesFieldsAndValues) {
    const auto raw = parse_raw(R"({
        "domains": [{
            "name": "example.com",
            "update_interval": 300,
            "driver": "test_driver",
            "subdomains": [{
                "name": "www", "type": "a", "ip_source": "http",
                "ip_source_param": "https://api.ipify.org",
                "driver_param": {"token": "secret-value", "ttl": 600, "proxied": true}
            }]
        }]
    })");
    const auto config = Config::normalize(raw);
    const auto& param = config.domains[0].subdomains[0].driver_param;

    // Opaque JSON text: must round-trip with every field intact.
    glz::generic reparsed;
    const auto ec = glz::read_json(reparsed, param);
    ASSERT_EQ(ec, glz::error_code::none) << glz::format_error(ec, param);
    const auto& obj = reparsed.get_object();
    EXPECT_EQ(obj.at("token").get<std::string>(), "secret-value");
    EXPECT_EQ(obj.at("ttl").get<double>(), 600.0);
    EXPECT_EQ(obj.at("proxied").get<bool>(), true);
}

// ===========================================================================
// Field pass-through
// ===========================================================================

TEST(NormalizerTest, SubdomainFields_PassedThrough) {
    auto raw = parse_raw(MINIMAL_CONFIG);
    auto& sub = raw.domains[0].subdomains[0];
    sub.type = RecordKind::AAAA;
    sub.interface = "eth0";
    sub.ip_type = AddressFamily::IPV6;
    sub.allow_ula = true;
    sub.allow_local_link = true;

    const auto config = Config::normalize(raw);
    const auto& out = config.domains[0].subdomains[0];
    EXPECT_EQ(out.name, "www");
    EXPECT_EQ(out.type, RecordKind::AAAA);
    EXPECT_EQ(out.interface, "eth0");
    EXPECT_EQ(out.ip_type, AddressFamily::IPV6);
    EXPECT_EQ(out.ip_source, Config::IpSource::HTTP);
    EXPECT_EQ(out.ip_source_param, "https://api.ipify.org");
    EXPECT_TRUE(out.allow_ula);
    EXPECT_TRUE(out.allow_local_link);
}

TEST(NormalizerTest, DomainFields_PassedThrough) {
    auto raw = parse_raw(MINIMAL_CONFIG);
    raw.domains[0].force_update = 600;

    const auto config = Config::normalize(raw);
    EXPECT_EQ(config.domains[0].name, "example.com");
    EXPECT_EQ(config.domains[0].update_interval, 300);
    EXPECT_EQ(config.domains[0].force_update, 600);
    EXPECT_EQ(config.domains[0].driver, "test_driver");
}

// ===========================================================================
// normalize() performs no validation
// ===========================================================================

TEST(NormalizerTest, StaticallyInvalidConfig_StillNormalizes) {
    // Interval below the production minimum: normalize() must not reject it
    // (scheduler/unit tests rely on small intervals; validation is separate).
    auto raw = parse_raw(MINIMAL_CONFIG);
    raw.domains[0].update_interval = 1;

    const auto config = Config::normalize(raw);
    EXPECT_EQ(config.domains[0].subdomains[0].update_interval, 1);
}

TEST(NormalizerTest, BootstrapDns_ParsedFromJsonIntoBootstrapServers) {
    const auto raw = parse_raw(R"({
        "driver": {},
        "resolver": { "use_custom_server": false },
        "domains": [],
        "bootstrap_dns": "9.9.9.9"
    })");
    EXPECT_EQ(raw.bootstrap_dns, "9.9.9.9");

    const auto config = Config::normalize(raw);
    ASSERT_EQ(config.resolver.bootstrap_servers.size(), 1U);
    EXPECT_EQ(config.resolver.bootstrap_servers[0].address, "9.9.9.9");
    EXPECT_EQ(config.resolver.bootstrap_servers[0].port, 53);
}

TEST(NormalizerTest, BootstrapDns_Unset_LeavesBootstrapServersEmpty) {
    const auto raw = parse_raw(MINIMAL_CONFIG);

    const auto config = Config::normalize(raw);
    EXPECT_TRUE(config.resolver.bootstrap_servers.empty());
}
