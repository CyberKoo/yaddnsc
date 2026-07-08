//
// Unit tests for config/config.h — Configuration data types.
//
// Verifies:
//   - Config::IpSource enum values.
//   - Config::ResolverStrategy enum values.
//   - Config structs (DriverConfig, ResolverConfig, SubdomainConfig,
//     DomainConfig, AppConfig) default values and aggregate initialisation.
// =============================================================================

#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "config/config.h"
#include "dns_type.h"
#include "address_family.h"

// ===========================================================================
// Config::IpSource
// ===========================================================================

TEST(ConfigIpSourceTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(Config::IpSource::INTERFACE), 0);
    EXPECT_EQ(static_cast<int>(Config::IpSource::HTTP), 1);
    EXPECT_EQ(static_cast<int>(Config::IpSource::MDNS), 2);
}

TEST(ConfigIpSourceTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<Config::IpSource>));
    EXPECT_FALSE((std::is_convertible_v<Config::IpSource, int>));
}

TEST(ConfigIpSourceTest, DefaultIsInterface) {
    Config::IpSource src{};
    EXPECT_EQ(src, Config::IpSource::INTERFACE);
}

// ===========================================================================
// Config::ResolverStrategy
// ===========================================================================

TEST(ConfigResolverStrategyTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(Config::ResolverStrategy::FALLBACK), 0);
    EXPECT_EQ(static_cast<int>(Config::ResolverStrategy::CONCURRENT), 1);
}

TEST(ConfigResolverStrategyTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<Config::ResolverStrategy>));
    EXPECT_FALSE((std::is_convertible_v<Config::ResolverStrategy, int>));
}

TEST(ConfigResolverStrategyTest, DefaultIsFallback) {
    Config::ResolverStrategy s{};
    EXPECT_EQ(s, Config::ResolverStrategy::FALLBACK);
}

// ===========================================================================
// Config::DriverConfig
// ===========================================================================

TEST(ConfigDriverConfigTest, DefaultValues) {
    Config::DriverConfig cfg{};
    EXPECT_FALSE(cfg.driver_dir.has_value());
    EXPECT_FALSE(cfg.auto_discover);
    EXPECT_TRUE(cfg.load.empty());
}

TEST(ConfigDriverConfigTest, AggregateInit) {
    Config::DriverConfig cfg{
        .driver_dir = "/custom/drivers",
        .auto_discover = true,
        .load = {"cloudflare", "digital_ocean"},
    };
    ASSERT_TRUE(cfg.driver_dir.has_value());
    EXPECT_EQ(*cfg.driver_dir, "/custom/drivers");
    EXPECT_TRUE(cfg.auto_discover);
    EXPECT_EQ(cfg.load.size(), 2U);
    EXPECT_EQ(cfg.load[0], "cloudflare");
    EXPECT_EQ(cfg.load[1], "digital_ocean");
}

// ===========================================================================
// Config::ResolverConfig
// ===========================================================================

TEST(ConfigResolverConfigTest, DefaultValues) {
    Config::ResolverConfig cfg{};
    EXPECT_FALSE(cfg.use_custom_server);
    EXPECT_TRUE(cfg.address.empty());
    EXPECT_EQ(cfg.port, 53);
    EXPECT_TRUE(cfg.servers.empty());
    EXPECT_EQ(cfg.strategy, Config::ResolverStrategy::CONCURRENT);
}

TEST(ConfigResolverConfigTest, AggregateInit) {
    Config::ResolverConfig cfg{
        .use_custom_server = true,
        .address = "1.1.1.1",
        .port = 853,
        .servers = {{"8.8.8.8", 53}},
        .strategy = Config::ResolverStrategy::FALLBACK,
    };
    EXPECT_TRUE(cfg.use_custom_server);
    EXPECT_EQ(cfg.address, "1.1.1.1");
    EXPECT_EQ(cfg.port, 853);
    ASSERT_EQ(cfg.servers.size(), 1U);
    EXPECT_EQ(cfg.servers[0].address, "8.8.8.8");
    EXPECT_EQ(cfg.servers[0].port, 53);
    EXPECT_EQ(cfg.strategy, Config::ResolverStrategy::FALLBACK);
}

// ===========================================================================
// Config::SubdomainConfig
// ===========================================================================

TEST(ConfigSubdomainConfigTest, DefaultValues) {
    Config::SubdomainConfig cfg{};
    EXPECT_TRUE(cfg.name.empty());
    EXPECT_EQ(cfg.type, RecordKind::A);
    EXPECT_TRUE(cfg.interface.empty());
    EXPECT_EQ(cfg.ip_type, AddressFamily::UNSPECIFIED);
    EXPECT_EQ(cfg.ip_source, Config::IpSource::INTERFACE);
    EXPECT_TRUE(cfg.ip_source_param.empty());
    EXPECT_FALSE(cfg.allow_ula);
    EXPECT_FALSE(cfg.allow_local_link);
    EXPECT_EQ(cfg.update_interval, 0);
}

TEST(ConfigSubdomainConfigTest, AggregateInit) {
    Config::SubdomainConfig cfg{
        .name = "www",
        .type = RecordKind::AAAA,
        .interface = "eth0",
        .ip_type = AddressFamily::IPV6,
        .ip_source = Config::IpSource::MDNS,
        .ip_source_param = "printer.local",
        .allow_ula = true,
        .allow_local_link = false,
        .update_interval = 60,
    };
    EXPECT_EQ(cfg.name, "www");
    EXPECT_EQ(cfg.type, RecordKind::AAAA);
    EXPECT_EQ(cfg.interface, "eth0");
    EXPECT_EQ(cfg.ip_type, AddressFamily::IPV6);
    EXPECT_EQ(cfg.ip_source, Config::IpSource::MDNS);
    EXPECT_EQ(cfg.ip_source_param, "printer.local");
    EXPECT_TRUE(cfg.allow_ula);
    EXPECT_FALSE(cfg.allow_local_link);
    EXPECT_EQ(cfg.update_interval, 60);
}

// ===========================================================================
// Config::DomainConfig
// ===========================================================================

TEST(ConfigDomainConfigTest, DefaultValues) {
    Config::DomainConfig cfg{};
    EXPECT_TRUE(cfg.name.empty());
    EXPECT_EQ(cfg.update_interval, 0);
    EXPECT_EQ(cfg.force_update, 0);
    EXPECT_TRUE(cfg.driver.empty());
    EXPECT_TRUE(cfg.subdomains.empty());
}

TEST(ConfigDomainConfigTest, AggregateInit) {
    Config::SubdomainConfig sub{
        .name = "@",
        .type = RecordKind::A,
    };

    Config::DomainConfig cfg{
        .name = "example.com",
        .update_interval = 300,
        .force_update = 3600,
        .driver = "cloudflare",
        .subdomains = {sub},
    };

    EXPECT_EQ(cfg.name, "example.com");
    EXPECT_EQ(cfg.update_interval, 300);
    EXPECT_EQ(cfg.force_update, 3600);
    EXPECT_EQ(cfg.driver, "cloudflare");
    ASSERT_EQ(cfg.subdomains.size(), 1U);
    EXPECT_EQ(cfg.subdomains[0].name, "@");
}

// ===========================================================================
// Config::AppConfig (top-level)
// ===========================================================================

TEST(ConfigAppConfigTest, DefaultValues) {
    // AppConfig has nested structs; default-init should zero everything.
    Config::AppConfig cfg{};
    EXPECT_FALSE(cfg.driver.auto_discover);
    EXPECT_FALSE(cfg.resolver.use_custom_server);
    EXPECT_TRUE(cfg.domains.empty());
}

TEST(ConfigAppConfigTest, AggregateInit) {
    Config::AppConfig cfg{
        .driver = {.driver_dir = "./drivers", .auto_discover = true},
        .resolver = {.use_custom_server = true, .servers = {{"9.9.9.9", 53}}},
        .domains = {
            {.name = "example.com", .update_interval = 300, .driver = "simple"},
        },
    };

    EXPECT_TRUE(cfg.driver.auto_discover);
    ASSERT_TRUE(cfg.driver.driver_dir.has_value());
    EXPECT_EQ(*cfg.driver.driver_dir, "./drivers");
    EXPECT_TRUE(cfg.resolver.use_custom_server);
    ASSERT_EQ(cfg.resolver.servers.size(), 1U);
    EXPECT_EQ(cfg.resolver.servers[0].address, "9.9.9.9");
    ASSERT_EQ(cfg.domains.size(), 1U);
    EXPECT_EQ(cfg.domains[0].name, "example.com");
}

// ===========================================================================
// Type traits
// ===========================================================================

TEST(ConfigTypeTraitsTest, ConfigStructsAreMoveable) {
    EXPECT_TRUE(std::is_move_constructible_v<Config::DriverConfig>);
    EXPECT_TRUE(std::is_move_assignable_v<Config::DriverConfig>);
    EXPECT_TRUE(std::is_move_constructible_v<Config::ResolverConfig>);
    EXPECT_TRUE(std::is_move_assignable_v<Config::ResolverConfig>);
    EXPECT_TRUE(std::is_move_constructible_v<Config::SubdomainConfig>);
    EXPECT_TRUE(std::is_move_assignable_v<Config::SubdomainConfig>);
    EXPECT_TRUE(std::is_move_constructible_v<Config::DomainConfig>);
    EXPECT_TRUE(std::is_move_assignable_v<Config::DomainConfig>);
    EXPECT_TRUE(std::is_move_constructible_v<Config::AppConfig>);
    EXPECT_TRUE(std::is_move_assignable_v<Config::AppConfig>);
}
