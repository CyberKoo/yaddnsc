//
// Unit tests for DNS::resolve_bootstrap — the no-network paths.
//
// Positive/family-filter paths against a live fake DNS server live in
// test/component/classic_resolver_test.cpp (BootstrapResolver suite).
// =============================================================================

#include "infrastructure/dns/bootstrap.h"

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "domain/config/dns_config.h"
#include "domain/error/dns_error.h"
#include "support/util/cancellation_token.hpp"

using namespace std::chrono_literals;

namespace {

const std::vector<Config::DnsServer> one_server{{"127.0.0.1", 21553}};

TEST(DnsBootstrap, EmptyServerList_ReturnsError) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    const auto result = DNS::resolve_bootstrap("example.com", std::nullopt, {}, deadline, {});
    ASSERT_FALSE(result.has_value());
}

TEST(DnsBootstrap, PreCancelledToken_ReturnsCancelled) {
    const Utils::CancellationSource source;
    source.trigger();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    const auto result = DNS::resolve_bootstrap("example.com", std::nullopt, one_server, deadline, source.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::CANCELLED);
}

TEST(DnsBootstrap, ExpiredDeadline_ReturnsRetry) {
    const auto deadline = std::chrono::steady_clock::now() - 1s;
    const auto result = DNS::resolve_bootstrap("example.com", std::nullopt, one_server, deadline, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::RETRY);
}

TEST(DnsBootstrap, UnreachableServer_ReportsFailure) {
    // UDP to a closed loopback port: the query either times out (RETRY) or
    // is refused (CONNECTION) — both must surface as an error, never a hang.
    const std::vector<Config::DnsServer> dead{{"127.0.0.1", 1}};
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    const auto result = DNS::resolve_bootstrap("example.com", std::nullopt, dead, deadline, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code == DnsError::RETRY || result.error().code == DnsError::CONNECTION);
}

}  // namespace
