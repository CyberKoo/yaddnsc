//
// Compile-time separation between the sync and async SDK interfaces. The two
// services/context families must not convert, and each exchange shape must be
// unusable through the other mode.

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <concepts>

#include <gtest/gtest.h>

// Both SDKs are included on purpose: the guard below only proves anything if
// the async SDK is checked against the synchronous one.
#include <yaddnsc/sdk/async_driver.hpp>  // IWYU pragma: keep
#ifdef YADDNSC_SDK_DRIVER_HPP
#error "Async SDK must not include synchronous driver entry machinery"
#endif
#include <yaddnsc/sdk/driver.hpp>  // IWYU pragma: keep
#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/log.hpp>

namespace sdk = yaddnsc::sdk;

namespace {

template<typename T>
concept SyncExchange = requires(const T& services, sdk::HttpRequest request) {
    { services.exchange(std::move(request)) } -> std::same_as<sdk::ExchangeResult>;
};

template<typename T>
concept AsyncExchange =
    requires(const T& services, sdk::HttpRequest request) { services.exchange(std::move(request)).await_ready(); };

static_assert(SyncExchange<sdk::Services>);
static_assert(!SyncExchange<sdk::AsyncServices>);
static_assert(AsyncExchange<sdk::AsyncServices>);
static_assert(!AsyncExchange<sdk::Services>);

static_assert(!std::is_convertible_v<sdk::Services, sdk::AsyncServices>);
static_assert(!std::is_convertible_v<sdk::AsyncServices, sdk::Services>);
static_assert(!std::is_constructible_v<sdk::AsyncServices, sdk::Services>);
static_assert(!std::is_constructible_v<sdk::Services, sdk::AsyncServices>);
static_assert(!std::is_constructible_v<sdk::AsyncUpdateContext, const sdk::UpdateRequest&, sdk::Services>);
static_assert(!std::is_constructible_v<sdk::UpdateContext, const sdk::UpdateRequest&, sdk::AsyncServices>);

}  // namespace

TEST(SdkAsyncSeparation, TheTwoInterfacesDoNotConvertOrShareAnExchangeShape) {
    SUCCEED();
}

TEST(SdkAsyncSeparation, NullLoggingTargetsNeverEvaluateArguments) {
    int evaluations = 0;
    YADDNSC_SDK_LOG_DEBUG(nullptr, "{}", ++evaluations);
    YADDNSC_ASYNC_SDK_LOG_DEBUG(nullptr, "{}", ++evaluations);
    EXPECT_EQ(evaluations, 0);
}
