//
// Contract tests for src/infrastructure/ip_source/adapter.cpp — IpSourceAdapter.
//
// Locks the translation from the legacy throwing IpSourceBase contract to
// the IpSourcePort error-value contract:
//   - resolve() success           → value, candidates passed through unchanged
//   - empty candidate list        → SUCCESS (empty), not an error
//   - std::exception from source  → {UNAVAILABLE, e.what()}
//   - non-standard throw          → {UNKNOWN, ...}
//   - exception from the factory  → {UNAVAILABLE, e.what()}
// =============================================================================

#include "infrastructure/ip_source/adapter.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <expected>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "infrastructure/ip_source/base.h"
#include "mocks/mock_ip_source.h"
#include "support/util/cancellation_token.hpp"

namespace {

using ::testing::Return;

[[nodiscard]] domain::SubdomainConfig any_subdomain_config() {
    return domain::SubdomainConfig{.name = "www", .type = RecordKind::A};
}

// Factory that always hands out the same preconfigured mock source.
template<typename F>
[[nodiscard]] IpSourceAdapter make_adapter(F&& factory_fn) {
    return IpSourceAdapter(Utils::CancellationToken{}, IpSourceAdapter::FactoryFn(std::forward<F>(factory_fn)));
}

}  // namespace

TEST(IpSourceAdapter, SuccessPassesCandidatesThrough) {
    auto source = std::make_unique<MockIpSource>();
    const std::vector<InetAddress> expected{InetAddress{Inet4Address::from_bytes({192, 0, 2, 1})},
                                            InetAddress{Inet4Address::from_bytes({198, 51, 100, 1})}};
    EXPECT_CALL(*source, resolve()).WillOnce(Return(expected));

    auto adapter = make_adapter([&](const domain::SubdomainConfig&) { return std::move(source); });
    const auto result = adapter.resolve(any_subdomain_config());

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2);
    EXPECT_EQ((*result)[0].to_string(), "192.0.2.1");
    EXPECT_EQ((*result)[1].to_string(), "198.51.100.1");
}

TEST(IpSourceAdapter, EmptyCandidatesAreSuccess) {
    auto source = std::make_unique<MockIpSource>();
    EXPECT_CALL(*source, resolve()).WillOnce(Return(std::vector<InetAddress>{}));

    auto adapter = make_adapter([&](const domain::SubdomainConfig&) { return std::move(source); });
    const auto result = adapter.resolve(any_subdomain_config());

    ASSERT_TRUE(result.has_value()) << "an empty candidate list is a success, not an error";
    EXPECT_TRUE(result->empty());
}

TEST(IpSourceAdapter, StdExceptionBecomesUnavailable) {
    auto source = std::make_unique<MockIpSource>();
    EXPECT_CALL(*source, resolve()).WillOnce([]() -> std::vector<InetAddress> {
        throw std::runtime_error("interface not found");
    });

    auto adapter = make_adapter([&](const domain::SubdomainConfig&) { return std::move(source); });
    const auto result = adapter.resolve(any_subdomain_config());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::UNAVAILABLE);
    EXPECT_EQ(result.error().message, "interface not found");
}

TEST(IpSourceAdapter, NonStandardThrowBecomesUnknown) {
    auto adapter = make_adapter([](const domain::SubdomainConfig&) -> std::unique_ptr<IpSourceBase> {
        class ThrowingSource final : public IpSourceBase {
        public:
            std::vector<InetAddress> resolve() const override { throw 42; }
        };
        return std::make_unique<ThrowingSource>();
    });
    const auto result = adapter.resolve(any_subdomain_config());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::UNKNOWN);
}

TEST(IpSourceAdapter, FactoryExceptionBecomesUnavailable) {
    auto adapter = make_adapter([](const domain::SubdomainConfig&) -> std::unique_ptr<IpSourceBase> {
        throw std::runtime_error("bad source config");
    });
    const auto result = adapter.resolve(any_subdomain_config());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::UNAVAILABLE);
    EXPECT_EQ(result.error().message, "bad source config");
}
