//
// Unit tests for src/dns/resolver_registry.cpp
//
// Verifies:
//   - register_factory + create for a registered schema.
//   - create with unknown schema throws DnsLookupException.
//   - create with empty schema uses fallback when registered.
//   - create with empty schema and no fallback throws (before any "" registration).
//   - Explicit unknown schema does NOT fall back to "".
//   - Registrar RAII helper.
//   - Multiple schemas resolve independently.
//
// NOTE: Because DnsResolverRegistry stores state in a function-local static
// (Meyer's singleton), test ordering within this suite matters. All scenarios
// are sequenced in a single TEST to guarantee predictable state.
// =============================================================================

#include <memory>
#include <expected>
#include <vector>
#include <cstdint>

#include <gtest/gtest.h>

#include "config/dns_config.h"
#include "dns/resolver/base.h"
#include "dns/resolver_registry.h"
#include "dns/dns_error_info.h"
#include "exception/dns_lookup.h"
#include "record_kind.h"
#include "util/cancellation_token.hpp"

// ── Minimal ResolverBase subclass for testing ───────────────────────────────

class TestResolver : public ResolverBase {
public:
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &, RecordKind, const Utils::CancellationToken &) const override {
        return std::vector<std::uint8_t>{};
    }

    [[nodiscard]] std::string_view get_type() const noexcept override { return "test"; }
};

// ── Helper to create a DnsServer ────────────────────────────────────────────

[[nodiscard]] Config::DnsServer make_server(std::string_view address, std::uint16_t port = 53) {
    return Config::DnsServer{std::string(address), port};
}

// ── All scenarios in a single ordered test ──────────────────────────────────

TEST(ResolverRegistryTest, AllScenarios_Ordered) {
    // ── 1. Unknown schema → throws ────────────────────────────────────────
    {
        auto server = make_server("unknown://resolver");
        EXPECT_THROW(
            { [[maybe_unused]] auto r = DnsResolverRegistry::create(server); },
            DnsLookupException
        );
    }

    // ── 2. Register a schema and create ──────────────────────────────────
    bool proto_called = false;
    DnsResolverRegistry::register_factory("proto", [&proto_called](const Config::DnsServer &s) {
        proto_called = true;
        EXPECT_EQ(s.address, "proto://host");
        EXPECT_EQ(s.port, 853);
        return std::make_unique<TestResolver>();
    });

    {
        auto server = make_server("proto://host", 853);
        auto resolver = DnsResolverRegistry::create(server);
        ASSERT_NE(resolver, nullptr);
        EXPECT_TRUE(proto_called);
        EXPECT_EQ(resolver->get_type(), "test");
    }

    // ── 3. Unknown schema still throws after registration ────────────────
    {
        auto server = make_server("other://resolver");
        EXPECT_THROW(
            { [[maybe_unused]] auto r = DnsResolverRegistry::create(server); },
            DnsLookupException
        );
    }

    // ── 4. Empty schema WITHOUT fallback → throws ────────────────────────
    // (no "" schema registered yet — but "127.0.0.1" has empty schema)
    {
        auto server = make_server("10.0.0.1");
        EXPECT_THROW(
            { [[maybe_unused]] auto r = DnsResolverRegistry::create(server); },
            DnsLookupException
        );
    }

    // ── 5. Register "" fallback → create with bare IP works ─────────────
    bool fallback_called = false;
    DnsResolverRegistry::register_factory("", [&fallback_called](const Config::DnsServer &s) {
        fallback_called = true;
        EXPECT_EQ(s.address, "192.168.1.1");
        return std::make_unique<TestResolver>();
    });

    {
        auto server = make_server("192.168.1.1");
        auto resolver = DnsResolverRegistry::create(server);
        ASSERT_NE(resolver, nullptr);
        EXPECT_TRUE(fallback_called);
        EXPECT_EQ(resolver->get_type(), "test");
    }

    // ── 6. Explicit unknown schema does NOT fall back to "" ─────────────
    {
        auto server = make_server("tls1://server");
        EXPECT_THROW(
            { [[maybe_unused]] auto r = DnsResolverRegistry::create(server); },
            DnsLookupException
        );
    }

    // ── 7. Multiple schemas resolve independently ────────────────────────
    DnsResolverRegistry::register_factory("alpha", [](const Config::DnsServer &) {
        return std::make_unique<TestResolver>();
    });
    DnsResolverRegistry::register_factory("beta", [](const Config::DnsServer &) {
        return std::make_unique<TestResolver>();
    });

    {
        auto ra = DnsResolverRegistry::create(make_server("alpha://srv"));
        auto rb = DnsResolverRegistry::create(make_server("beta://srv"));
        ASSERT_NE(ra, nullptr);
        ASSERT_NE(rb, nullptr);
    }
}

// ── Registrar RAII helper ──────────────────────────────────────────────────

TEST(ResolverRegistryTest, Registrar_RegistersFactory) {
    {
        DnsResolverRegistry::Registrar _reg("raii", [](const Config::DnsServer &) {
            return std::make_unique<TestResolver>();
        });

        auto server = make_server("raii://host");
        auto resolver = DnsResolverRegistry::create(server);
        ASSERT_NE(resolver, nullptr);
    }
}
