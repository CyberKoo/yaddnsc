//
// Contract tests for src/infrastructure/ip_source/adapter.cpp — IpSourceAdapter.
//
// Locks composition of the source and factory structured-result contracts:
//   - resolve() success           → value, candidates passed through unchanged
//   - empty candidate list        → SUCCESS (empty), not an error
//   - source failures             → passed through unchanged
//   - factory failures            → passed through unchanged
//   - unexpected implementation exception → {UNKNOWN, ...}
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

using ::testing::_;
using ::testing::Return;

[[nodiscard]] domain::SubdomainConfig any_subdomain_config() {
    return domain::SubdomainConfig{.name = "www", .type = RecordKind::A};
}

// Factory that always hands out the same preconfigured mock source.
template<typename F>
[[nodiscard]] IpSourceAdapter make_adapter(F&& factory_fn) {
    return IpSourceAdapter(IpSourceAdapter::FactoryFn(std::forward<F>(factory_fn)));
}

}  // namespace

TEST(IpSourceAdapter, SuccessPassesCandidatesThrough) {
    auto source = std::make_unique<MockIpSource>();
    const std::vector<InetAddress> expected{InetAddress{Inet4Address::from_bytes({192, 0, 2, 1})},
                                            InetAddress{Inet4Address::from_bytes({198, 51, 100, 1})}};
    EXPECT_CALL(*source, resolve(_)).WillOnce(Return(IpSourceBase::Result{expected}));

    auto adapter = make_adapter([&](const domain::SubdomainConfig&) { return std::move(source); });
    const auto result = adapter.resolve(any_subdomain_config(), {});

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2);
    EXPECT_EQ((*result)[0].to_string(), "192.0.2.1");
    EXPECT_EQ((*result)[1].to_string(), "198.51.100.1");
}

TEST(IpSourceAdapter, EmptyCandidatesAreSuccess) {
    auto source = std::make_unique<MockIpSource>();
    EXPECT_CALL(*source, resolve(_)).WillOnce(Return(IpSourceBase::Result{std::vector<InetAddress>{}}));

    auto adapter = make_adapter([&](const domain::SubdomainConfig&) { return std::move(source); });
    const auto result = adapter.resolve(any_subdomain_config(), {});

    ASSERT_TRUE(result.has_value()) << "an empty candidate list is a success, not an error";
    EXPECT_TRUE(result->empty());
}

TEST(IpSourceAdapter, SourceFailurePassesThrough) {
    auto source = std::make_unique<MockIpSource>();
    EXPECT_CALL(*source, resolve(_))
        .WillOnce(Return(std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE, "interface not found"})));

    auto adapter = make_adapter([&](const domain::SubdomainConfig&) { return std::move(source); });
    const auto result = adapter.resolve(any_subdomain_config(), {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::UNAVAILABLE);
    EXPECT_EQ(result.error().message, "interface not found");
}

TEST(IpSourceAdapter, UnexpectedSourceExceptionBecomesUnknown) {
    auto adapter = make_adapter([](const domain::SubdomainConfig&) -> IpSourceFactory::Result {
        class ThrowingSource final : public IpSourceBase {
        public:
            Result resolve(const Utils::CancellationToken&) const override { throw 42; }
        };
        return std::make_unique<ThrowingSource>();
    });
    const auto result = adapter.resolve(any_subdomain_config(), {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::UNKNOWN);
}

TEST(IpSourceAdapter, FactoryFailurePassesThrough) {
    auto adapter = make_adapter([](const domain::SubdomainConfig&) -> IpSourceFactory::Result {
        return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE, "bad source config"});
    });
    const auto result = adapter.resolve(any_subdomain_config(), {});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::UNAVAILABLE);
    EXPECT_EQ(result.error().message, "bad source config");
}
