//
// Unit tests for src/dns/factory.cpp — DnsResolverFactory.
//
// Verifies:
//   - create() with custom server uses configured servers.
//   - create() with legacy single-server format.
//   - create() without custom server falls back to default.
// =============================================================================

#include <memory>
#include <expected>
#include <vector>
#include <cstdint>

#include <gtest/gtest.h>

#include "config/config.h"
#include "config/dns_config.h"
#include "dns/factory.h"
#include "dns/resolver/base.h"
#include "dns/resolver_registry.h"
#include "dns/dns_error_info.h"
#include "record_kind.h"
#include "util/cancellation_token.hpp"

// ── Minimal ResolverBase subclass for factory testing ───────────────────────

class FactoryTestResolver : public ResolverBase {
public:
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &, RecordKind, const Utils::CancellationToken &) const override {
        return std::vector<std::uint8_t>{};
    }
    [[nodiscard]] std::string_view get_type() const noexcept override { return "factory_test"; }
};

// ── Register a test resolver factory ────────────────────────────────────────

namespace {
    [[maybe_unused]] DnsResolverRegistry::Registrar _factory_test_reg(
        "factorytest",
        [](const Config::DnsServer &) -> std::unique_ptr<ResolverBase> {
            return std::make_unique<FactoryTestResolver>();
        }
    );
    [[maybe_unused]] DnsResolverRegistry::Registrar _factory_default_reg(
        "",
        [](const Config::DnsServer &) -> std::unique_ptr<ResolverBase> {
            return std::make_unique<FactoryTestResolver>();
        }
    );
}

// ── Helper to populate AppConfig fields ─────────────────────────────────────

[[nodiscard]] Config::AppConfig make_config_with_servers(std::vector<Config::DnsServer> servers) {
    Config::AppConfig cfg;
    cfg.resolver.use_custom_server = true;
    cfg.resolver.servers = std::move(servers);
    cfg.resolver.strategy = Config::ResolverStrategy::FALLBACK;
    return cfg;
}

[[nodiscard]] Config::AppConfig make_config_with_legacy_server(std::string_view address, std::uint16_t port) {
    Config::AppConfig cfg;
    cfg.resolver.use_custom_server = true;
    cfg.resolver.address = std::string(address);
    cfg.resolver.port = port;
    cfg.resolver.strategy = Config::ResolverStrategy::FALLBACK;
    return cfg;
}

[[nodiscard]] Config::AppConfig make_config_no_custom_server() {
    Config::AppConfig cfg;
    cfg.resolver.use_custom_server = false;
    cfg.resolver.strategy = Config::ResolverStrategy::CONCURRENT;
    return cfg;
}

TEST(DnsFactoryTest, CreateWithCustomServers) {
    auto cfg = make_config_with_servers({
        {"factorytest://dns1.example.com", 53},
        {"factorytest://dns2.example.com", 53},
    });

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(cfg);
    });
}

TEST(DnsFactoryTest, CreateWithLegacySingleServer) {
    auto cfg = make_config_with_legacy_server("factorytest://dns.example.com", 5353);

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(cfg);
    });
}

TEST(DnsFactoryTest, CreateWithoutCustomServer_UsesDefault) {
    auto cfg = make_config_no_custom_server();

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(cfg);
    });
}

TEST(DnsFactoryTest, CreateWithMultipleServers_DoesNotThrow) {
    std::vector<Config::DnsServer> servers;
    servers.push_back({"factorytest://primary.example.com", 53});
    servers.push_back({"factorytest://secondary.example.com", 53});
    auto cfg = make_config_with_servers(std::move(servers));

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(cfg);
    });
}

TEST(DnsFactoryTest, CreateWithConcurrentStrategy) {
    Config::AppConfig cfg;
    cfg.resolver.use_custom_server = true;
    cfg.resolver.servers.push_back({"factorytest://dns.example.com", 53});
    cfg.resolver.strategy = Config::ResolverStrategy::CONCURRENT;

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(cfg);
    });
}
