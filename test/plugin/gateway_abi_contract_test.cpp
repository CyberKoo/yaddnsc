//
// ABI error-report contract tests for the coroutine driver gateway.
//
// These drive a deliberately contract-violating plugin (fixtures/abi_misbehave.cpp)
// through the real gateway: a dlopen'ed module, a create → update/validate →
// destroy cycle on an offload worker, the full yaddnsc_status → DriverError
// mapping and the has_valid_plugin_error() guard.
//
// The plugin breaks exactly one ABI rule per case, selected by name, so each
// assertion pins the host's reaction to that single violation: a report the
// host cannot trust must be refused as an invalid ABI result rather than mapped
// as if it were true.
//
// No mode reaches the host HTTP bridge, so no server, socket or DNS is involved.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; the coroutines here use EXPECT_* only.
//

#include <chrono>
#include <climits>
#include <coroutine>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "domain/error/error.h"
#include "domain/update/driver_update_command.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_gateway.h"
#include "infrastructure/plugin/shared_library.h"
#include "mocks/null_logger.h"

namespace app {
class LoggerPort;
}  // namespace app

namespace {

using namespace std::chrono_literals;

constexpr std::string_view PLUGIN_PATH = ABI_MISBEHAVE_PATH;
const std::string DRIVER_NAME = "abi_misbehave";

/// The fixture's call counters, read through its test-only exports.
struct PluginCounts {
    std::uint64_t creates = 0;
    std::uint64_t updates = 0;
    std::uint64_t validates = 0;
    std::uint64_t destroys = 0;
};

/// Drives the fixture's test-only exports. The module is already mapped by the
/// catalog, so this second handle reaches the same globals.
class MisbehaveControl {
public:
    using GetState = void (*)(std::uint64_t*, std::uint64_t*, std::uint64_t*, std::uint64_t*);

    explicit MisbehaveControl(const std::string& path) {
        auto library = SharedLibrary::open(path);
        if (!library.has_value()) {
            return;
        }
        library_ = std::move(*library);
        set_mode_ = reinterpret_cast<int (*)(const char*)>(library_.resolve("abi_misbehave_set_mode"));  // NOLINT
        reset_ = reinterpret_cast<void (*)()>(library_.resolve("abi_misbehave_reset_state"));            // NOLINT
        get_state_ = reinterpret_cast<GetState>(library_.resolve("abi_misbehave_get_state"));             // NOLINT
    }

    /// Whether every test-only export resolved — a false here would silently
    /// turn every mode into the well-behaved default.
    [[nodiscard]] bool ready() const noexcept { return set_mode_ != nullptr && get_state_ != nullptr; }

    /// Returns the fixture's own result: 0 when the mode name was recognised,
    /// non-zero otherwise. Propagating it keeps a mistyped mode from reading as
    /// a passing case.
    [[nodiscard]] int set_mode(const std::string_view name) const { return set_mode_(name.data()); }

    void reset() const { reset_(); }

    [[nodiscard]] PluginCounts state() const {
        PluginCounts counts;
        get_state_(&counts.creates, &counts.updates, &counts.validates, &counts.destroys);
        return counts;
    }

private:
    SharedLibrary library_;
    int (*set_mode_)(const char*) = nullptr;
    void (*reset_)() = nullptr;
    GetState get_state_ = nullptr;
};

[[nodiscard]] domain::DriverUpdateCommand make_command() {
    return domain::DriverUpdateCommand{
        .driver_params = R"({"op":"noop"})",
        .ip_addr = "192.0.2.1",
        .rd_type = "A",
        .domain = "example.com",
        .subdomain = "www",
        .fqdn = "www.example.com",
    };
}

[[nodiscard]] plugin::DriverGateway::Options default_options() {
    plugin::DriverGateway::Options options;
    options.bridge_wait_budget = 3000ms;
    return options;
}

/// Run one update cycle of the gateway over the loaded catalog.
void run_update(const DriverCatalog& catalog, const app::LoggerPort& logger,
                std::expected<void, domain::DriverError>& result) {
    coro::Loop loop;
    coro::run(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog, logger, loop, group, default_options());
            result = co_await gateway.update(DRIVER_NAME, make_command());
            co_return;
        });
        co_return;
    }());
}

/// Run one validate_config cycle of the gateway over the loaded catalog.
void run_validate(const DriverCatalog& catalog, const app::LoggerPort& logger,
                  std::expected<void, domain::DriverError>& result) {
    coro::Loop loop;
    coro::run(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog, logger, loop, group, default_options());
            result = co_await gateway.validate_config(DRIVER_NAME, R"({"key":"value"})");
            co_return;
        });
        co_return;
    }());
}

/// The plugin is loaded once per test and returned to the well-behaved mode, so
/// a violation cannot leak into the next case through the module's globals.
class GatewayAbiContractTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NO_THROW(catalog_.load_driver(std::string(PLUGIN_PATH)));
        control_ = std::make_unique<MisbehaveControl>(std::string(PLUGIN_PATH));
        ASSERT_TRUE(control_->ready()) << "fixture test-only exports did not resolve";
        control_->reset();
    }

    /// Select the single ABI violation under test; the fixture returns non-zero
    /// for an unknown name, which would otherwise read as a passing case.
    void select_mode(const std::string_view name) {
        ASSERT_EQ(control_->set_mode(name), 0) << "fixture rejected mode name: " << name;
    }

    DriverCatalog catalog_;
    NullLogger logger_;
    std::unique_ptr<MisbehaveControl> control_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Baseline: a well-behaved plugin still succeeds, so every assertion below is
// about the violation and not about a broken harness.
// ---------------------------------------------------------------------------

TEST_F(GatewayAbiContractTest, WellBehavedPlugin_SucceedsAndRunsTheWholeCycle) {
    select_mode("well_behaved");

    std::expected<void, domain::DriverError> result;
    run_update(catalog_, logger_, result);

    ASSERT_TRUE(result.has_value()) << result.error().message;
    const auto counts = control_->state();
    EXPECT_EQ(counts.creates, 1U);
    EXPECT_EQ(counts.updates, 1U);
    EXPECT_EQ(counts.destroys, 1U);
}

TEST_F(GatewayAbiContractTest, UnknownModeName_IsRejectedRatherThanIgnored) {
    // Guards the guard: a mistyped mode must not silently read as "well
    // behaved", which would make every violation case below pass vacuously.
    EXPECT_NE(control_->set_mode("no_such_mode"), 0);
}

// ---------------------------------------------------------------------------
// create() contract
// ---------------------------------------------------------------------------

TEST_F(GatewayAbiContractTest, CreateReturnsOkWithoutAHandle_IsAnInvalidAbiResult) {
    select_mode("create_ok_without_handle");

    std::expected<void, domain::DriverError> result;
    run_update(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_NE(result.error().message.find("create"), std::string::npos)
        << result.error().message;
    // The host must never destroy an instance it never received.
    const auto counts = control_->state();
    EXPECT_EQ(counts.creates, 1U);
    EXPECT_EQ(counts.destroys, 0U);
}

TEST_F(GatewayAbiContractTest, CreateFailsWithoutAnErrorReport_IsAnInvalidAbiResult) {
    select_mode("create_fails_without_report");

    std::expected<void, domain::DriverError> result;
    run_update(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_NE(result.error().message.find("invalid"), std::string::npos)
        << result.error().message;
}

TEST_F(GatewayAbiContractTest, CreateReportStatusDisagreesWithTheReturnValue_IsAnInvalidAbiResult) {
    select_mode("create_fails_with_stale_status");

    std::expected<void, domain::DriverError> result;
    run_update(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_NE(result.error().message.find("create"), std::string::npos)
        << result.error().message;
}

TEST_F(GatewayAbiContractTest, CreateReportsAFailureWithNoMessage_TheHostSynthesisesOne) {
    select_mode("create_reports_empty_message");

    std::expected<void, domain::DriverError> result;
    run_update(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    // An empty message is a legal report, so this is a mapped failure rather
    // than an invalid one — and the host still has to produce readable text.
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_NE(result.error().message.find(DRIVER_NAME), std::string::npos)
        << result.error().message;
    EXPECT_NE(result.error().message.find("www.example.com"), std::string::npos)
        << result.error().message;
}

// ---------------------------------------------------------------------------
// update() contract and the status mapping
// ---------------------------------------------------------------------------

TEST_F(GatewayAbiContractTest, UpdateFailsWithoutAnErrorReport_IsAnInvalidAbiResult) {
    select_mode("update_fails_without_report");

    std::expected<void, domain::DriverError> result;
    run_update(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_NE(result.error().message.find("update"), std::string::npos)
        << result.error().message;
    // A refused report still leaves the instance owned by the host: destroy ran.
    const auto counts = control_->state();
    EXPECT_EQ(counts.updates, 1U);
    EXPECT_EQ(counts.destroys, 1U);
}

TEST_F(GatewayAbiContractTest, UpdateReportsNetworkFailure_MapsToUpdateFailed) {
    select_mode("update_reports_network_error");

    std::expected<void, domain::DriverError> result;
    run_update(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UPDATE_FAILED);
    EXPECT_EQ(result.error().message, "plugin reported this failure");
}

TEST_F(GatewayAbiContractTest, UpdateReportsRateLimited_MapsToRateLimitedAndClampsRetryAfter) {
    select_mode("update_reports_rate_limited");

    std::expected<void, domain::DriverError> result;
    run_update(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::RATE_LIMITED);
    EXPECT_EQ(result.error().message, "plugin reported this failure");
    // The plugin sent UINT32_MAX; the host must saturate at INT_MAX rather than
    // narrowing into a negative delay.
    EXPECT_EQ(result.error().retry_after_seconds, INT_MAX);
}

TEST_F(GatewayAbiContractTest, UpdateReportsInvalidArgument_MapsToUnknown) {
    // A valid status with no case in the mapping is a plugin-side contract bug;
    // the host still maps it rather than leaving it unhandled.
    select_mode("update_reports_invalid_argument");

    std::expected<void, domain::DriverError> result;
    run_update(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_EQ(result.error().message, "plugin reported this failure");
}

// ---------------------------------------------------------------------------
// validate_config contract
// ---------------------------------------------------------------------------

TEST_F(GatewayAbiContractTest, ValidateFailsWithoutAnErrorReport_IsAnInvalidAbiResult) {
    select_mode("validate_fails_without_report");

    std::expected<void, domain::DriverError> result;
    run_validate(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_NE(result.error().message.find("validate"), std::string::npos)
        << result.error().message;
    const auto counts = control_->state();
    EXPECT_EQ(counts.validates, 1U);
    EXPECT_EQ(counts.destroys, 1U);
}

TEST_F(GatewayAbiContractTest, ValidateRejectsConfig_MapsToAValidationError) {
    select_mode("validate_rejects_config");

    std::expected<void, domain::DriverError> result;
    run_validate(catalog_, logger_, result);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_EQ(result.error().message, "plugin reported this failure");
}
