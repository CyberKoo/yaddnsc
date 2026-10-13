//
// Unit tests for the composition assembly helpers (src/composition/assembly.cpp).
//
// The helpers here are value-level: log-level tokens, error joining, the two
// option bundles the commands hand to their adapters, and the bootstrap-server
// short circuit. They take their inputs by value, so every case below is pure
// in-memory logic — no sockets, no files, no plugin loading.
//
// Deliberately not exercised (see the coverage report for the reasons):
//   - make_default_tls_context(): OpenSSL CA discovery and bundle load are real
//     filesystem I/O, and its failure branch additionally needs a host with no
//     usable trust store.
//   - fill_bootstrap_servers() on an empty list: the fallback parses
//     /etc/resolv.conf.
//   - load_runtime_config() / load_catalog(): they read a configuration file
//     and dlopen driver plugins; both belong in the component suite.
// =============================================================================

#include "composition/assembly.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "infrastructure/dns/policy.h"
#include "infrastructure/http/types.h"

#include "version.h"

namespace Composition::internal {
/// assembly.cpp's own bootstrap helper. It is internal to the composition root
/// and deliberately absent from assembly.h, so the test re-declares the
/// production signature to drive the "configured bootstrap_dns wins" branch
/// without going through a configuration file.
void fill_bootstrap_servers(domain::RuntimeConfig& config);
}  // namespace Composition::internal

// ===========================================================================
//  to_log_level — the --log-level vocabulary
// ===========================================================================

TEST(CompositionAssembly, ToLogLevel_MapsEachAcceptedToken) {
    EXPECT_EQ(Composition::internal::to_log_level("trace"), spdlog::level::trace);
    EXPECT_EQ(Composition::internal::to_log_level("debug"), spdlog::level::debug);
    EXPECT_EQ(Composition::internal::to_log_level("info"), spdlog::level::info);
    EXPECT_EQ(Composition::internal::to_log_level("warn"), spdlog::level::warn);
}

TEST(CompositionAssembly, ToLogLevel_UnknownTokensFallBackToErrorSeverity) {
    // The CLI restricts --log-level to error/warn/info/debug/trace, so the
    // fallthrough is unreachable in production; the mapping still has to pick
    // the quietest severity rather than anything more verbose, and must not
    // treat the token case- or whitespace-insensitively.
    const std::array tokens{"err", "error", "warning", "TRACE", "Debug", "", " ", "trace "};
    for (const std::string_view token : tokens) {
        SCOPED_TRACE(token);
        EXPECT_EQ(Composition::internal::to_log_level(token), spdlog::level::err);
    }
}

// ===========================================================================
//  format_config_errors — collect-all validation reporting
// ===========================================================================

TEST(CompositionAssembly, FormatConfigErrors_NoErrorsJoinsToNothing) {
    const std::vector<domain::ConfigError> errors;
    EXPECT_TRUE(Composition::internal::format_config_errors(errors).empty());
}

TEST(CompositionAssembly, FormatConfigErrors_SingleErrorHasNoSeparator) {
    const std::vector<domain::ConfigError> errors{{domain::ConfigError::Code::EMPTY_DOMAINS, "no domains configured"}};
    EXPECT_EQ(Composition::internal::format_config_errors(errors), "no domains configured");
}

TEST(CompositionAssembly, FormatConfigErrors_EveryErrorIsReportedOnItsOwnLine) {
    // One command reports all problems, so the join must not drop or reorder
    // entries: the caller sees them in validation order.
    const std::vector<domain::ConfigError> errors{
        {domain::ConfigError::Code::EMPTY_DOMAINS, "no domains configured"},
        {domain::ConfigError::Code::INTERFACE_NOT_FOUND, "interface eth-missing not found"},
        {domain::ConfigError::Code::DRIVER_NOT_FOUND, "driver cloudflare not loaded"},
    };
    EXPECT_EQ(Composition::internal::format_config_errors(errors),
              "no domains configured\ninterface eth-missing not found\ndriver cloudflare not loaded");
}

// ===========================================================================
//  make_gateway_options — the driver-gateway bundle
// ===========================================================================

TEST(CompositionAssembly, MakeGatewayOptions_KeepsHttpPolicyAndSetsTheBridgeBudget) {
    http::Options http;
    http.user_agent = "yaddnsc/unit-test";
    http.max_redirects = 3;

    const auto options = Composition::internal::make_gateway_options(http);

    EXPECT_EQ(options.http.user_agent, "yaddnsc/unit-test");
    EXPECT_EQ(options.http.max_redirects, 3);
    EXPECT_EQ(options.bridge_wait_budget, std::chrono::seconds(5));
}

// ===========================================================================
//  make_coro_http_options — the coroutine HTTP bundle
// ===========================================================================

TEST(CompositionAssembly, MakeCoroHttpOptions_StampsVersionAndInjectsTheResolver) {
    domain::ResolverSettings resolver;
    resolver.bootstrap_servers = {domain::DnsServer{.address = "192.0.2.53", .port = 53},
                                  domain::DnsServer{.address = "2001:db8::53", .port = 53}};

    const auto options = Composition::internal::make_coro_http_options(resolver, nullptr);

    EXPECT_EQ(options.user_agent, std::string{YADDNSC::get_full_version()});
    // A populated resolver callable is what makes hostname targets work at all:
    // /etc/hosts and NSS are never consulted, so an empty one would fail every
    // outbound connect that is not an IP literal.
    EXPECT_TRUE(static_cast<bool>(options.resolve));
    EXPECT_EQ(options.tls_context, nullptr);
}

TEST(CompositionAssembly, MakeCoroHttpOptions_LeavesTheTransportDefaultsAlone) {
    // Assembly supplies the identity, the resolver and the trust context; every
    // other knob keeps the library default instead of being silently tuned.
    const http::Options defaults;

    const auto options = Composition::internal::make_coro_http_options(domain::ResolverSettings{}, nullptr);

    EXPECT_EQ(options.version, defaults.version);
    EXPECT_EQ(options.keep_alive, defaults.keep_alive);
    EXPECT_EQ(options.follow_redirects, defaults.follow_redirects);
    EXPECT_EQ(options.max_redirects, defaults.max_redirects);
    EXPECT_EQ(options.connect_timeout, defaults.connect_timeout);
    EXPECT_EQ(options.queue_timeout, defaults.queue_timeout);
    EXPECT_EQ(options.address_family, defaults.address_family);
    EXPECT_EQ(options.factory, defaults.factory);
}

// ===========================================================================
//  fill_bootstrap_servers — configured bootstrap_dns wins
// ===========================================================================

TEST(CompositionAssembly, FillBootstrapServers_ConfiguredBootstrapDnsIsKeptVerbatim) {
    // The short circuit is what keeps a configured bootstrap_dns authoritative:
    // without it the helper would overwrite the user's choice with whatever
    // /etc/resolv.conf happens to list.
    domain::RuntimeConfig config;
    config.resolver.bootstrap_servers = {domain::DnsServer{.address = "192.0.2.53", .port = 5353},
                                         domain::DnsServer{.address = "198.51.100.7", .port = 53}};

    Composition::internal::fill_bootstrap_servers(config);

    ASSERT_EQ(config.resolver.bootstrap_servers.size(), 2u);
    EXPECT_EQ(config.resolver.bootstrap_servers[0].address, "192.0.2.53");
    EXPECT_EQ(config.resolver.bootstrap_servers[0].port, std::uint16_t{5353});
    EXPECT_EQ(config.resolver.bootstrap_servers[1].address, "198.51.100.7");
    EXPECT_EQ(config.resolver.bootstrap_servers[1].port, std::uint16_t{53});
}

TEST(CompositionAssembly, FillBootstrapServers_LeavesTheRestOfTheConfigUntouched) {
    domain::RuntimeConfig config;
    config.resolver.bootstrap_servers = {domain::DnsServer{.address = "192.0.2.53", .port = 53}};
    config.resolver.servers = {domain::DnsServer{.address = "9.9.9.9", .port = 53}};
    config.resolver.strategy = domain::ResolverStrategy::FALLBACK;

    Composition::internal::fill_bootstrap_servers(config);

    ASSERT_EQ(config.resolver.servers.size(), 1u);
    EXPECT_EQ(config.resolver.servers.front().address, "9.9.9.9");
    EXPECT_EQ(config.resolver.strategy, domain::ResolverStrategy::FALLBACK);
    EXPECT_TRUE(config.domains.empty());
}

// ===========================================================================
//  Shared production defaults
// ===========================================================================

TEST(CompositionAssembly, ConfigValidationBudgetIsTheDocumentedFiveSeconds) {
    EXPECT_EQ(Composition::internal::CONFIG_VALIDATION_BUDGET, std::chrono::milliseconds(5000));
}

TEST(CompositionAssembly, UpdatePolicyCarriesTheSharedValidationBudget) {
    // `run` and `config test` must apply the same deadline to the same plugin
    // call, so the budget is injected once and never defaulted downstream.
    EXPECT_EQ(Composition::internal::UPDATE_POLICY.driver_validation_budget,
              Composition::internal::CONFIG_VALIDATION_BUDGET);
}

TEST(CompositionAssembly, DnsPolicyIsTheLibraryDefault) {
    // One policy object goes to both make_coro_http_options and make_dispatcher:
    // the HTTP hostname resolver and the dispatcher must schedule the same way.
    const dns::Policy defaults;
    EXPECT_EQ(Composition::internal::DNS_POLICY.attempt_budget, defaults.attempt_budget);
    EXPECT_EQ(Composition::internal::DNS_POLICY.connect_budget, defaults.connect_budget);
    EXPECT_EQ(Composition::internal::DNS_POLICY.wire_budget, defaults.wire_budget);
    EXPECT_EQ(Composition::internal::DNS_POLICY.family_collection_window, defaults.family_collection_window);
    EXPECT_EQ(Composition::internal::DNS_POLICY.retry_backoff, defaults.retry_backoff);
    EXPECT_EQ(Composition::internal::DNS_POLICY.max_retries, defaults.max_retries);
    EXPECT_EQ(Composition::internal::DNS_POLICY.max_concurrent, defaults.max_concurrent);
}