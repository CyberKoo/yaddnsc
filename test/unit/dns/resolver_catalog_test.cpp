//
// Unit tests for src/infrastructure/dns/resolver_catalog.cpp — ResolverCatalog.
//
// Instance-level successor of the removed global DnsResolverRegistry tests:
// each test builds its own catalog, so the suite is order-independent and
// parallel-safe (the registry test needed a single ordered TEST because the
// registry was a function-local static).
//
// Verifies:
//   - register_factory + create for a registered schema.
//   - create with unknown schema throws DnsLookupException (same message).
//   - create with empty schema uses the "" fallback when registered.
//   - create with empty schema and no fallback throws.
//   - Explicit unknown schema does NOT fall back to "".
//   - Multiple schemas resolve independently.
//   - with_builtins() dispatches "" / "https" / "tls" to the built-in
//     resolver types (construction only — no network I/O).
// =============================================================================

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "domain/config/dns_config.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/resolver/base.h"
#include "infrastructure/dns/resolver_catalog.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "domain/dns/record_kind.h"
#include "support/util/cancellation_token.hpp"

// ── Minimal ResolverBase subclass for testing ───────────────────────────────

class TestResolver : public ResolverBase {
public:
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &, RecordKind) const override {
        return std::vector<std::uint8_t>{};
    }

    [[nodiscard]] std::string_view get_type() const noexcept override { return "test"; }
};

// ── Helper to create a DnsServer ────────────────────────────────────────────

[[nodiscard]] Config::DnsServer make_server(std::string_view address, std::uint16_t port = 53) {
    return Config::DnsServer{std::string(address), port};
}

TEST(ResolverCatalogTest, UnknownSchemaThrows) {
    const ResolverCatalog catalog;
    EXPECT_THROW(
        { [[maybe_unused]] auto r = catalog.create(make_server("unknown://resolver"), {}); },
        DnsLookupException
    );
}

TEST(ResolverCatalogTest, UnknownSchemaErrorMessage) {
    const ResolverCatalog catalog;
    try {
        [[maybe_unused]] auto r = catalog.create(make_server("tls1://server"), {});
        FAIL() << "expected DnsLookupException";
    } catch (const DnsLookupException &e) {
        EXPECT_STREQ(e.what(), R"(No resolver factory registered for schema "tls1" (server: tls1://server))");
    }
}

TEST(ResolverCatalogTest, RegisterAndCreate) {
    ResolverCatalog catalog;
    bool proto_called = false;
    catalog.register_factory("proto", [&proto_called](const Config::DnsServer &s, const Utils::CancellationToken &) {
        proto_called = true;
        EXPECT_EQ(s.address, "proto://host");
        EXPECT_EQ(s.port, 853);
        return std::make_unique<TestResolver>();
    });

    auto resolver = catalog.create(make_server("proto://host", 853), {});
    ASSERT_NE(resolver, nullptr);
    EXPECT_TRUE(proto_called);
    EXPECT_EQ(resolver->get_type(), "test");
}

TEST(ResolverCatalogTest, UnknownSchemaStillThrowsAfterRegistration) {
    ResolverCatalog catalog;
    catalog.register_factory("proto", [](const Config::DnsServer &, const Utils::CancellationToken &) {
        return std::make_unique<TestResolver>();
    });

    EXPECT_THROW(
        { [[maybe_unused]] auto r = catalog.create(make_server("other://resolver"), {}); },
        DnsLookupException
    );
}

TEST(ResolverCatalogTest, EmptySchemaWithoutFallbackThrows) {
    ResolverCatalog catalog;
    catalog.register_factory("proto", [](const Config::DnsServer &, const Utils::CancellationToken &) {
        return std::make_unique<TestResolver>();
    });

    // "10.0.0.1" has an empty schema and no "" factory is registered.
    EXPECT_THROW(
        { [[maybe_unused]] auto r = catalog.create(make_server("10.0.0.1"), {}); },
        DnsLookupException
    );
}

TEST(ResolverCatalogTest, EmptySchemaFallsBackToDefault) {
    ResolverCatalog catalog;
    bool fallback_called = false;
    catalog.register_factory("", [&fallback_called](const Config::DnsServer &s, const Utils::CancellationToken &) {
        fallback_called = true;
        EXPECT_EQ(s.address, "192.168.1.1");
        return std::make_unique<TestResolver>();
    });

    auto resolver = catalog.create(make_server("192.168.1.1"), {});
    ASSERT_NE(resolver, nullptr);
    EXPECT_TRUE(fallback_called);
    EXPECT_EQ(resolver->get_type(), "test");
}

TEST(ResolverCatalogTest, ExplicitUnknownSchemaDoesNotFallBack) {
    ResolverCatalog catalog;
    catalog.register_factory("", [](const Config::DnsServer &, const Utils::CancellationToken &) {
        return std::make_unique<TestResolver>();
    });

    EXPECT_THROW(
        { [[maybe_unused]] auto r = catalog.create(make_server("tls1://server"), {}); },
        DnsLookupException
    );
}

TEST(ResolverCatalogTest, MultipleSchemasResolveIndependently) {
    ResolverCatalog catalog;
    catalog.register_factory("alpha", [](const Config::DnsServer &, const Utils::CancellationToken &) {
        return std::make_unique<TestResolver>();
    });
    catalog.register_factory("beta", [](const Config::DnsServer &, const Utils::CancellationToken &) {
        return std::make_unique<TestResolver>();
    });

    auto ra = catalog.create(make_server("alpha://srv"), {});
    auto rb = catalog.create(make_server("beta://srv"), {});
    ASSERT_NE(ra, nullptr);
    ASSERT_NE(rb, nullptr);
}

TEST(ResolverCatalogTest, CatalogsAreIndependentInstances) {
    ResolverCatalog first;
    first.register_factory("", [](const Config::DnsServer &, const Utils::CancellationToken &) {
        return std::make_unique<TestResolver>();
    });

    // A second catalog does not see the first one's registrations.
    const ResolverCatalog second;
    EXPECT_THROW(
        { [[maybe_unused]] auto r = second.create(make_server("192.168.1.1"), {}); },
        DnsLookupException
    );
}

// ── with_builtins — schema dispatch to the real resolver types ──────────────
// Construction performs no network I/O: ClassicResolver only parses the URI,
// DoH/DoT create their TLS stream objects lazily (connect happens on query).

TEST(ResolverCatalogTest, Builtins_ClassicForBareAddress) {
    const auto catalog = ResolverCatalog::with_builtins();
    auto resolver = catalog.create(make_server("1.1.1.1", 53), {});
    ASSERT_NE(resolver, nullptr);
    EXPECT_EQ(resolver->get_type(), "Classic");
}

TEST(ResolverCatalogTest, Builtins_DohForHttpsSchema) {
    const auto catalog = ResolverCatalog::with_builtins();
    auto resolver = catalog.create(make_server("https://1.1.1.1/dns-query"), {});
    ASSERT_NE(resolver, nullptr);
    EXPECT_EQ(resolver->get_type(), "DNS-Over-HTTPS");
}

TEST(ResolverCatalogTest, Builtins_DotForTlsSchema) {
    const auto catalog = ResolverCatalog::with_builtins();
    auto resolver = catalog.create(make_server("tls://1.1.1.1"), {});
    ASSERT_NE(resolver, nullptr);
    EXPECT_EQ(resolver->get_type(), "DNS-Over-TLS");
}

TEST(ResolverCatalogTest, Builtins_UnknownSchemaThrows) {
    const auto catalog = ResolverCatalog::with_builtins();
    EXPECT_THROW(
        { [[maybe_unused]] auto r = catalog.create(make_server("quic://dns.example"), {}); },
        DnsLookupException
    );
}
