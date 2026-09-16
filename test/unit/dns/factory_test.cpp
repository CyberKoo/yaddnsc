//
// Unit tests for src/dns/factory.cpp — DnsResolverFactory.
//
// Verifies:
//   - create() with configured servers.
//   - create() with an empty server list falls back to the built-in default.
// (Legacy single-server folding now lives in the config normaliser and is
// covered by normalizer_test.)
// =============================================================================

#include <memory>
#include <expected>
#include <vector>
#include <cstdint>

#include <gtest/gtest.h>

#include "config/dns_config.h"
#include "domain/config/runtime_config.h"
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
    query(const std::string &, RecordKind) const override {
        return std::vector<std::uint8_t>{};
    }
    [[nodiscard]] std::string_view get_type() const noexcept override { return "factory_test"; }
};

// ── Register a test resolver factory ────────────────────────────────────────

namespace {
    [[maybe_unused]] DnsResolverRegistry::Registrar _factory_test_reg(
        "factorytest",
        [](const Config::DnsServer &, const Utils::CancellationToken &) -> std::unique_ptr<ResolverBase> {
            return std::make_unique<FactoryTestResolver>();
        }
    );
    [[maybe_unused]] DnsResolverRegistry::Registrar _factory_default_reg(
        "",
        [](const Config::DnsServer &, const Utils::CancellationToken &) -> std::unique_ptr<ResolverBase> {
            return std::make_unique<FactoryTestResolver>();
        }
    );
}

// ── Helper to populate resolver settings ────────────────────────────────────

[[nodiscard]] domain::ResolverSettings make_settings(std::vector<Config::DnsServer> servers,
                                                     Config::ResolverStrategy strategy) {
    domain::ResolverSettings settings;
    settings.servers = std::move(servers);
    settings.strategy = strategy;
    return settings;
}

TEST(DnsFactoryTest, CreateWithCustomServers) {
    auto settings = make_settings({
        {"factorytest://dns1.example.com", 53},
        {"factorytest://dns2.example.com", 53},
    }, Config::ResolverStrategy::FALLBACK);

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(settings, {});
    });
}

TEST(DnsFactoryTest, CreateWithEmptyServerList_UsesDefault) {
    const domain::ResolverSettings settings;

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(settings, {});
    });
}

TEST(DnsFactoryTest, CreateWithMultipleServers_DoesNotThrow) {
    std::vector<Config::DnsServer> servers;
    servers.push_back({"factorytest://primary.example.com", 53});
    servers.push_back({"factorytest://secondary.example.com", 53});
    auto settings = make_settings(std::move(servers), Config::ResolverStrategy::FALLBACK);

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(settings, {});
    });
}

TEST(DnsFactoryTest, CreateWithConcurrentStrategy) {
    auto settings = make_settings({{"factorytest://dns.example.com", 53}},
                                  Config::ResolverStrategy::CONCURRENT);

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(settings, {});
    });
}

TEST(DnsFactoryTest, CreateWithShuffleStrategy) {
    auto settings = make_settings({
        {"factorytest://dns1.example.com", 53},
        {"factorytest://dns2.example.com", 53},
    }, Config::ResolverStrategy::SHUFFLE);

    EXPECT_NO_THROW({
        auto dispatcher = DnsResolverFactory::create(settings, {});
    });
}
