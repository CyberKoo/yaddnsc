/// Frozen ABI 1.0 compatibility tests — the freeze tripwire.
///
/// The frozen plugin (abi_baseline/frozen_v1_0_plugin.cpp) compiles against
/// the frozen v1.0 header copy, never the live header. These tests drive it
/// through the production PluginModule: a host-side change that breaks ABI
/// 1.0 compatibility turns them red even though every test built against the
/// live header still passes.
///
/// See docs/development.md#plugin-abi-changes.

#include <cstdint>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "infrastructure/plugin/plugin_loader.h"
#include "plugin/plugin_test_doubles.h"

namespace {

constexpr std::string_view FROZEN_PLUGIN_PATH = FROZEN_V1_0_FIXTURE;

}  // namespace

TEST(AbiFreezeCompat, FrozenV10PluginLoadsAgainstCurrentHost) {
    auto module = PluginModule::load(std::string(FROZEN_PLUGIN_PATH));
    ASSERT_TRUE(module.has_value()) << module.error().message;

    const auto& descriptor = module->descriptor();
    EXPECT_EQ(descriptor.name, "frozen_v1_0");
    EXPECT_EQ(descriptor.version, "1.0.0");
    EXPECT_EQ(descriptor.abi_major, UINT16_C(1));
    EXPECT_EQ(descriptor.abi_minor, UINT16_C(0));
    EXPECT_EQ(descriptor.capabilities, YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA);
    EXPECT_TRUE(module->supports_validate());
}

TEST(AbiFreezeCompat, FullUpdateCycleThroughFrozenAbi) {
    auto module = PluginModule::load(std::string(FROZEN_PLUGIN_PATH));
    ASSERT_TRUE(module.has_value()) << module.error().message;

    HostUpdateContext host;
    host.client.queue_response(204, "", {});
    const auto services = host.services();

    const auto result = run_module_cycle(*module, services, R"({"token":"t"})");
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.update_status, YADDNSC_STATUS_OK) << result.error_message;

    // The plugin really used the host services: exactly one exchange with the
    // request it built, and at least one log record carrying its source
    // location.
    ASSERT_EQ(host.client.request_count(), 1u);
    EXPECT_EQ(host.client.requests().front().url, "https://frozen-v1-0.example.com/nic/update");
    EXPECT_EQ(host.client.requests().front().method, TestHttpMethod::GET);
    ASSERT_FALSE(host.logger.records().empty());
    EXPECT_EQ(host.logger.records().front().message, "frozen ABI 1.0 plugin updating");
    EXPECT_EQ(host.logger.records().front().file, "frozen_v1_0_plugin.cpp");
    EXPECT_GT(host.logger.records().front().line, 0);
}

TEST(AbiFreezeCompat, ExchangeFailurePropagatesThroughFrozenAbi) {
    auto module = PluginModule::load(std::string(FROZEN_PLUGIN_PATH));
    ASSERT_TRUE(module.has_value()) << module.error().message;

    HostUpdateContext host;
    host.client.queue_error(false, "connection refused", 0);
    const auto services = host.services();

    const auto result = run_module_cycle(*module, services, R"({"token":"t"})");
    ASSERT_EQ(result.update_status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(result.error_message, "connection refused");
}

TEST(AbiFreezeCompat, ValidateEntryThroughFrozenAbi) {
    auto module = PluginModule::load(std::string(FROZEN_PLUGIN_PATH));
    ASSERT_TRUE(module.has_value()) << module.error().message;

    // The validate table refuses HTTP, exactly like config test's table.
    HostUpdateContext host;
    const auto services = host.context.make_services(false);

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_driver* handle = nullptr;
    ASSERT_EQ(module->create(services, &handle, error), YADDNSC_STATUS_OK);

    constexpr std::string_view VALID_JSON = R"({"token":"t"})";
    const yaddnsc_string valid{VALID_JSON.data(), VALID_JSON.size()};
    EXPECT_EQ(module->validate(handle, valid, error), YADDNSC_STATUS_OK);

    const yaddnsc_string empty{};
    EXPECT_EQ(module->validate(handle, empty, error), YADDNSC_STATUS_INVALID_CONFIG);

    module->destroy(handle);
}
