//
// Unit tests for the coroutine DNS factory — the configured-address →
// backend mapping. No sockets: make_dispatcher only builds the backend
// objects, so address parsing is asserted through what it accepts and what
// it rejects.
//

#include "infrastructure/dns/factory.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "domain/config/dns_config.h"
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/static_validator.h"
#include "infrastructure/uri/uri.h"

namespace {

[[nodiscard]] domain::ResolverSettings settings_with(std::vector<domain::DnsServer> servers) {
    domain::ResolverSettings settings;
    settings.servers = std::move(servers);
    return settings;
}

/// Build a dispatcher holding a single configured server.
[[nodiscard]] std::unique_ptr<dns::Dispatcher> build(const domain::DnsServer& server) {
    return dns::make_dispatcher(settings_with({server}), {}, nullptr);
}

TEST(DnsFactoryTest, StaticValidationAndFactoryAgreeOnEndpoints) {
    for (const auto& server : std::vector<domain::DnsServer>{{"1.1.1.1", 53},
                                                             {"[::1]", 53},
                                                             {"::1", 53},
                                                             {"2606:4700:4700::1111", 53},
                                                             {"1.1.1.1:53", 53},
                                                             {"[::1]:53", 53},
                                                             {"1.1.1.1", 0},
                                                             {"1.1.1.1/path", 53},
                                                             {"1.1.1.1?", 53},
                                                             {"1.1.1.1#x", 53},
                                                             {"tls://dns.test", 853},
                                                             {"tls://dns.test:8853", 853},
                                                             {"tls://dns.test/", 853},
                                                             {"tls://dns.test:0", 853},
                                                             {"tls://dns.test?", 853},
                                                             {"tls://dns.test#x", 853},
                                                             {"https://dns.test/dns-query", 53},
                                                             {"https://dns.test", 443},
                                                             {"https://dns.test:0/dns-query", 53},
                                                             {"ftp://dns.test", 53}}) {
        SCOPED_TRACE(server.address);
        Config::AppConfig raw;
        raw.resolver.use_custom_servers = true;
        raw.resolver.servers = {server};
        const auto errors = Config::validate_static(raw);
        const bool rejected = std::ranges::any_of(
            errors, [](const auto& error) { return error.code == domain::ConfigError::Code::INVALID_RESOLVER; });
        bool factory_rejected = false;
        try {
            [[maybe_unused]] const auto dispatcher = build(server);
        } catch (const std::invalid_argument&) {
            factory_rejected = true;
        }
        EXPECT_EQ(rejected, factory_rejected);
    }
}

TEST(DnsFactoryTest, DirectFactoryRejectsZeroPortsAndIgnoredComponents) {
    for (const auto& server : std::vector<domain::DnsServer>{{"127.0.0.1", 0},
                                                             {"127.0.0.1:53", 53},
                                                             {"127.0.0.1?", 53},
                                                             {"tls://dns.test:0", 853},
                                                             {"tls://dns.test/", 853},
                                                             {"tls://dns.test/path", 853},
                                                             {"tls://dns.test?", 853},
                                                             {"tls://dns.test#fragment", 853}}) {
        EXPECT_THROW({ [[maybe_unused]] const auto dispatcher = build(server); }, std::invalid_argument);
    }
    EXPECT_NO_THROW({ [[maybe_unused]] const auto dispatcher = build({"tls://dns.test", 853}); });
}

// The factory carries no port fallback of its own any more: a "tls" endpoint
// written without a port is completed by Uri's well-known port map. This pins
// that contract, so losing the "tls" entry there fails here instead of the
// resolver dialling port 0 in production.
TEST(DnsFactoryTest, DotWithoutExplicitPort_UsesTheWellKnownTlsPort) {
    const auto uri = Uri::parse("tls://dns.test");
    ASSERT_TRUE(uri.has_value());
    EXPECT_EQ(uri->get_port(), 853);
    EXPECT_NO_THROW({ [[maybe_unused]] const auto dispatcher = build({"tls://dns.test", 853}); });
}

TEST(DnsFactoryTest, ClassicIPv4_BuildsBackend) {
    EXPECT_NO_THROW({ [[maybe_unused]] auto dispatcher = build({.address = "1.1.1.1", .port = 53}); });
}

// Regression: the factory used to read the host through get_host_literal(),
// which brackets IPv6 for URL building. "[2606:4700:4700::1111]" then
// reached inet_pton with its brackets and the valid address was rejected as
// "not an IP literal", aborting `yaddnsc run` with an unhandled exception.
TEST(DnsFactoryTest, ClassicBareIPv6_BuildsBackend) {
    EXPECT_NO_THROW({ [[maybe_unused]] auto dispatcher = build({.address = "2606:4700:4700::1111", .port = 53}); });
}

TEST(DnsFactoryTest, ClassicBracketedIPv6_BuildsBackend) {
    EXPECT_NO_THROW({ [[maybe_unused]] auto dispatcher = build({.address = "[2606:4700:4700::1111]", .port = 53}); });
}

TEST(DnsFactoryTest, ClassicLoopbackIPv6_BuildsBackend) {
    EXPECT_NO_THROW({ [[maybe_unused]] auto dispatcher = build({.address = "::1", .port = 53}); });
}

TEST(DnsFactoryTest, ClassicHostname_Throws) {
    EXPECT_THROW(
        { [[maybe_unused]] auto dispatcher = build({.address = "resolver.example.com", .port = 53}); },
        std::invalid_argument);
}

TEST(DnsFactoryTest, UnknownSchema_Throws) {
    EXPECT_THROW(
        { [[maybe_unused]] auto dispatcher = build({.address = "ftp://1.1.1.1", .port = 53}); }, std::invalid_argument);
}

TEST(DnsFactoryTest, EmptyServerList_Throws) {
    EXPECT_THROW(
        { [[maybe_unused]] auto dispatcher = dns::make_dispatcher(settings_with({}), {}, nullptr); },
        std::invalid_argument);
}

TEST(DnsFactoryTest, BackendPerConfiguredServer) {
    const auto dispatcher = dns::make_dispatcher(
        settings_with({{"1.1.1.1", 53}, {"2606:4700:4700::1111", 53}, {"[2606:4700:4700::1111]", 53}}), {}, nullptr);
    ASSERT_NE(dispatcher, nullptr);
    EXPECT_EQ(dispatcher->size(), 3U);
}

}  // namespace
