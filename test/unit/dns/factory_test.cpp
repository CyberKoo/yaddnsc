//
// Unit tests for src/infrastructure/dns/factory.cpp — DnsResolverFactory.
//
// Verifies:
//   - create() with configured servers.
//   - create() with an empty server list falls back to the built-in default.
// (Legacy single-server folding now lives in the config normaliser and is
// covered by normalizer_test.)
//
// The resolver catalog is injected per test — no global registry, so the
// suite is parallel-safe.
// =============================================================================

#include "infrastructure/dns/factory.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "domain/config/dns_config.h"
#include "domain/config/runtime_config.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/resolver/base.h"
#include "infrastructure/dns/resolver_catalog.h"

// ── Minimal ResolverBase subclass for factory testing ───────────────────────

class FactoryTestResolver : public ResolverBase {
public:
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query(const std::string&,
                                                                               RecordKind,
                                                                               const Utils::CancellationToken&) const override {
        return std::vector<std::uint8_t>{};
    }

    [[nodiscard]] std::string_view get_type() const noexcept override { return "factory_test"; }
};

// ── Stub catalog: "factorytest" schema + "" fallback ────────────────────────

namespace {
[[nodiscard]] ResolverCatalog make_stub_catalog() {
    ResolverCatalog catalog;
    const ResolverCatalog::FactoryFn factory = [](const Config::DnsServer&) -> std::unique_ptr<ResolverBase> {
        return std::make_unique<FactoryTestResolver>();
    };
    catalog.register_factory("factorytest", factory);
    catalog.register_factory("", factory);
    return catalog;
}
}  // namespace

// ── Helper to populate resolver settings ────────────────────────────────────

[[nodiscard]] domain::ResolverSettings make_settings(std::vector<Config::DnsServer> servers,
                                                     Config::ResolverStrategy strategy) {
    domain::ResolverSettings settings;
    settings.servers = std::move(servers);
    settings.strategy = strategy;
    return settings;
}

TEST(DnsFactoryTest, CreateWithCustomServers) {
    auto settings = make_settings(
        {
            {"factorytest://dns1.example.com", 53},
            {"factorytest://dns2.example.com", 53},
        },
        Config::ResolverStrategy::FALLBACK);

    EXPECT_NO_THROW({ auto dispatcher = DnsResolverFactory::create(settings, make_stub_catalog()); });
}

TEST(DnsFactoryTest, CreateWithEmptyServerList_UsesDefault) {
    const domain::ResolverSettings settings;

    EXPECT_NO_THROW({ auto dispatcher = DnsResolverFactory::create(settings, make_stub_catalog()); });
}

TEST(DnsFactoryTest, CreateWithMultipleServers_DoesNotThrow) {
    std::vector<Config::DnsServer> servers;
    servers.push_back({"factorytest://primary.example.com", 53});
    servers.push_back({"factorytest://secondary.example.com", 53});
    auto settings = make_settings(std::move(servers), Config::ResolverStrategy::FALLBACK);

    EXPECT_NO_THROW({ auto dispatcher = DnsResolverFactory::create(settings, make_stub_catalog()); });
}

TEST(DnsFactoryTest, CreateWithConcurrentStrategy) {
    auto settings = make_settings({{"factorytest://dns.example.com", 53}}, Config::ResolverStrategy::CONCURRENT);

    EXPECT_NO_THROW({ auto dispatcher = DnsResolverFactory::create(settings, make_stub_catalog()); });
}

TEST(DnsFactoryTest, CreateWithShuffleStrategy) {
    auto settings = make_settings(
        {
            {"factorytest://dns1.example.com", 53},
            {"factorytest://dns2.example.com", 53},
        },
        Config::ResolverStrategy::SHUFFLE);

    EXPECT_NO_THROW({ auto dispatcher = DnsResolverFactory::create(settings, make_stub_catalog()); });
}

TEST(DnsFactoryTest, UnknownSchemaThrows) {
    auto settings = make_settings({{"nosuchproto://dns.example.com", 53}}, Config::ResolverStrategy::FALLBACK);

    EXPECT_THROW(
        { auto dispatcher = DnsResolverFactory::create(settings, make_stub_catalog()); }, DnsLookupException);
}
