//
// Compile-time separation between the sync and async SDK interfaces. The two
// services/context families must not convert, and each exchange shape must be
// unusable through the other mode.

#include <concepts>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver.hpp>

namespace sdk = yaddnsc::sdk;

namespace {

template<typename T>
concept SyncExchange = requires(const T& services, sdk::HttpRequest request) {
    { services.exchange(std::move(request)) } -> std::same_as<sdk::ExchangeResult>;
};

template<typename T>
concept AsyncExchange = requires(const T& services, sdk::HttpRequest request) {
    services.exchange(std::move(request)).await_ready();
};

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
