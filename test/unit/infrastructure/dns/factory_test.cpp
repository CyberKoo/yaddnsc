//
// Unit tests for the coroutine DNS factory — the configured-address →
// backend mapping. No sockets: make_dispatcher only builds the backend
// objects, so address parsing is asserted through what it accepts and what
// it rejects.
//

#include "infrastructure/dns/factory.h"

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "domain/config/dns_config.h"
#include "domain/config/runtime_config.h"

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
