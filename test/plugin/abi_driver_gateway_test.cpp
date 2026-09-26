//
// Created by Kotarou on 2026/9/17.
//

/// AbiDriverGateway contract tests: yaddnsc_status → domain::DriverError
/// mapping, error message / retry_after pass-through, the empty-message
/// fallback wording, driver-not-found wording, one HttpClient per update,
/// and cancellation visibility — all against the real dlopen'ed whiteboard
/// test plugin with a scripted HttpClient behind the factory.
///
/// The validate_config suite additionally loads the "no_validate" fixture (a
/// complete plugin predating the OPTIONAL validate entry) to lock the
/// skip-instead-of-fail behaviour.

#include "infrastructure/plugin/abi_driver_gateway.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "application/ports/driver_gateway.h"
#include "domain/error/error.h"
#include "infrastructure/network/http/client_port.h"
#include "infrastructure/network/http/error.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/shared_library.h"
#include "plugin/plugin_test_doubles.h"
#include "support/util/cancellation_token.hpp"

namespace {

constexpr std::string_view kPluginPath = TEST_PLUGIN_PATH;
constexpr std::string_view kNoValidatePluginPath = NO_VALIDATE_FIXTURE;
constexpr std::string_view kDriverName = "test_driver_plugin";
constexpr std::string_view kNoValidateDriverName = "no_validate";
constexpr std::string_view kFqdn = "www.example.com";

/// Triggers the operation token after the first completed transport call.
/// It lets the real test plugin prove it observes cancellation before it
/// begins its next exchange.
class TriggerAfterFirstExchangeClient final : public HttpClient {
public:
    TriggerAfterFirstExchangeClient(std::shared_ptr<QueueHttpClient> inner, const Utils::CancellationSource& source)
        : inner_(std::move(inner)), source_(source) {}

    [[nodiscard]] std::expected<net::http::Response, net::http::Error>
    exchange(std::string_view url,
             const net::http::Request& request,
             const Utils::CancellationToken& token) const override {
        auto result = inner_->exchange(url, request, token);
        if (exchange_count_.fetch_add(1, std::memory_order_relaxed) == 0) {
            source_.trigger();
        }
        return result;
    }

private:
    std::shared_ptr<QueueHttpClient> inner_;
    const Utils::CancellationSource& source_;
    mutable std::atomic<uint32_t> exchange_count_{0};
};

/// Test fixture: catalog with the whiteboard plugin, a shared scripted HTTP
/// queue behind the per-update factory, and a recording logger.
class AbiDriverGatewayTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NO_THROW(catalog_.load_driver(std::string(kPluginPath)));
        queue_ = std::make_shared<QueueHttpClient>();
        factory_calls_ = 0;
        gateway_ = std::make_unique<AbiDriverGateway>(
            catalog_,
            [this]() -> std::unique_ptr<HttpClient> {
                ++factory_calls_;
                return std::make_unique<SharedHttpClient>(queue_);
            },
            logger_);
    }

    [[nodiscard]] DriverUpdateCommand make_command(std::string driver_param) const {
        return DriverUpdateCommand{
            .driver_param = std::move(driver_param),
            .ip_addr = "192.0.2.1",
            .rd_type = "A",
            .domain = "example.com",
            .subdomain = "www",
            .fqdn = std::string(kFqdn),
        };
    }

    DriverCatalog catalog_;
    std::shared_ptr<QueueHttpClient> queue_;
    RecordingLogger logger_;
    Utils::CancellationSource cancel_source_;
    int factory_calls_ = 0;
    std::unique_ptr<AbiDriverGateway> gateway_;
};

}  // namespace

TEST_F(AbiDriverGatewayTest, UnknownDriverReportsNotFound) {
    const auto result = gateway_->update("missing", make_command(R"({"op":"success"})"), cancel_source_.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::NOT_FOUND);
    EXPECT_EQ(result.error().message, "Driver 'missing' is not loaded");
}

TEST_F(AbiDriverGatewayTest, SuccessfulUpdate) {
    const auto result = gateway_->update(kDriverName, make_command(R"({"op":"success"})"), cancel_source_.token());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(AbiDriverGatewayTest, StatusMapping) {
    const std::vector<std::pair<std::string, domain::DriverError::Code>> cases = {
        {"network_error", domain::DriverError::Code::UPDATE_FAILED},
        {"authentication_failed", domain::DriverError::Code::UPDATE_FAILED},
        {"upstream_rejected", domain::DriverError::Code::UPDATE_FAILED},
        {"unsupported_record", domain::DriverError::Code::UPDATE_FAILED},
        {"invalid_response", domain::DriverError::Code::UPDATE_FAILED},
        {"invalid_config", domain::DriverError::Code::UNKNOWN},
        {"internal_error", domain::DriverError::Code::UNKNOWN},
        {"cancelled", domain::DriverError::Code::CANCELLED},
        {"rate_limited", domain::DriverError::Code::RATE_LIMITED},
    };

    for (const auto& [status, expected_code] : cases) {
        const auto result = gateway_->update(
            kDriverName, make_command(R"({"op":"fail","status":")" + status + R"(","message":"m-)" + status + R"("})"),
            cancel_source_.token());
        ASSERT_FALSE(result.has_value()) << status;
        EXPECT_EQ(result.error().code, expected_code) << status;
        EXPECT_EQ(result.error().message, "m-" + status) << status;
        EXPECT_EQ(result.error().retry_after_seconds, 0) << status;
    }
}

TEST_F(AbiDriverGatewayTest, RetryAfterIsPassedThrough) {
    const auto result = gateway_->update(
        kDriverName, make_command(R"({"op":"fail","status":"rate_limited","message":"slow","retry_after":120})"),
        cancel_source_.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::RATE_LIMITED);
    EXPECT_EQ(result.error().message, "slow");
    EXPECT_EQ(result.error().retry_after_seconds, 120);
}

TEST_F(AbiDriverGatewayTest, OversizedRetryAfterIsClamped) {
    // The ABI field is uint32; a value beyond INT_MAX must saturate instead
    // of narrowing to a negative backoff.
    const auto result = gateway_->update(
        kDriverName,
        make_command(R"({"op":"fail","status":"rate_limited","message":"slow","retry_after":4294967295})"),
        cancel_source_.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().retry_after_seconds, std::numeric_limits<int>::max());
}

TEST_F(AbiDriverGatewayTest, TransportRetryAfterIsPassedThrough) {
    queue_->queue_error(net::http::ErrorCode::CONNECT_FAILED, "back off", 45);

    const auto result =
        gateway_->update(kDriverName, make_command(R"({"op":"exchange","http_count":1})"), cancel_source_.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UPDATE_FAILED);
    EXPECT_EQ(result.error().message, "exchange failed: back off");
    EXPECT_EQ(result.error().retry_after_seconds, 45);
}

TEST_F(AbiDriverGatewayTest, EmptyPluginMessageFallsBackToLegacyWording) {
    const auto result = gateway_->update(
        kDriverName, make_command(R"({"op":"fail","status":"network_error","message":""})"), cancel_source_.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UPDATE_FAILED);
    EXPECT_EQ(result.error().message, "Driver 'test_driver_plugin' update failed for www.example.com");
}

TEST_F(AbiDriverGatewayTest, OneHttpClientPerUpdate) {
    // Three exchanges inside a single update must share the one Client the
    // factory produced for this update (observable through the scripted
    // queue).
    queue_->queue_response(200, "a");
    queue_->queue_response(200, "b");
    queue_->queue_response(200, "c");

    const auto result =
        gateway_->update(kDriverName, make_command(R"({"op":"exchange","http_count":3})"), cancel_source_.token());
    EXPECT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(factory_calls_, 1);
    EXPECT_EQ(queue_->request_count(), 3u);
}

TEST_F(AbiDriverGatewayTest, PluginLogReachesTheHostLogger) {
    const auto result = gateway_->update(kDriverName,
                                         make_command(R"({"op":"log_macro","message":"via gateway"})"),
                                         cancel_source_.token());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto records = logger_.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message, "via gateway");
    EXPECT_TRUE(records[0].file.ends_with("test_driver_plugin.cpp")) << records[0].file;
    EXPECT_EQ(records[0].function, "update");
}

TEST_F(AbiDriverGatewayTest, OperationCancellationIsVisibleToThePlugin) {
    cancel_source_.trigger();
    const auto result = gateway_->update(kDriverName,
                                         make_command(R"({"op":"check_cancel","expect_cancelled":true})"),
                                         cancel_source_.token());
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(AbiDriverGatewayTest, CancellationBetweenExchangesStopsTheNextNetworkOperation) {
    gateway_ = std::make_unique<AbiDriverGateway>(
        catalog_,
        [this]() -> std::unique_ptr<HttpClient> {
            return std::make_unique<TriggerAfterFirstExchangeClient>(queue_, cancel_source_);
        },
        logger_);
    queue_->queue_response(200, "first");
    queue_->queue_response(200, "must not be requested");

    const auto result =
        gateway_->update(kDriverName, make_command(R"({"op":"exchange","http_count":2})"), cancel_source_.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::CANCELLED);
    EXPECT_EQ(queue_->request_count(), 1u);
    EXPECT_EQ(queue_->remaining(), 1u);
}

TEST_F(AbiDriverGatewayTest, UpdateParametersReachThePlugin) {
    const auto result = gateway_->update(kDriverName, make_command(R"({"op":"echo_params"})"), cancel_source_.token());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    const auto records = logger_.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message,
              R"(params ip=192.0.2.1 rd=A domain=example.com sub=www fqdn=www.example.com param={"op":"echo_params"})");
}

TEST(AbiDriverGatewayCreateTest, CreateFailureAfterStoringHandleDoesNotLeak) {
    // The leaky_create fixture stores a handle and then fails create; the
    // gateway must still surface the failure AND the leaked handle must have
    // been destroyed by the host backstop — no instance escapes.
    DriverCatalog catalog;
    ASSERT_NO_THROW(catalog.load_driver(std::string(LEAKY_CREATE_FIXTURE)));

    auto control_library = SharedLibrary::open(LEAKY_CREATE_FIXTURE);
    ASSERT_TRUE(control_library.has_value()) << control_library.error();
    using SetMode = void (*)(int);
    using GetState = void (*)(uint64_t*, uintptr_t*);
    const auto set_mode = reinterpret_cast<SetMode>(control_library->resolve("leaky_create_set_mode"));      // NOLINT
    const auto get_state = reinterpret_cast<GetState>(control_library->resolve("leaky_create_get_state"));  // NOLINT
    ASSERT_NE(set_mode, nullptr);
    ASSERT_NE(get_state, nullptr);
    set_mode(0);

    RecordingLogger logger;
    const AbiDriverGateway gateway(
        catalog, []() -> std::unique_ptr<HttpClient> { return std::make_unique<QueueHttpClient>(); }, logger);
    const Utils::CancellationSource cancellation;

    const auto result = gateway.update("leaky_create",
                                       DriverUpdateCommand{
                                           .driver_param = "{}",
                                           .ip_addr = "192.0.2.1",
                                           .rd_type = "A",
                                           .domain = "example.com",
                                           .subdomain = "www",
                                           .fqdn = "www.example.com",
                                       },
                                       cancellation.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_EQ(result.error().message, "create failed after storing a handle");

    uint64_t destroys = 0;
    uintptr_t last_destroyed = 0;
    get_state(&destroys, &last_destroyed);
    EXPECT_EQ(destroys, 1u);
    EXPECT_NE(last_destroyed, 0u);
}

// ── validate_config ──────────────────────────────────────────────────────────
//
// The host's `config test` path. Three outcomes are locked here: a valid
// driver_param passes, a driver-side rejection surfaces the plugin's message
// verbatim, and a plugin without the OPTIONAL validate entry is skipped.

namespace {

/// Catalog holding both the whiteboard plugin (validate entry present) and
/// the "no_validate" fixture (no validate entry). validate_config performs no
/// HTTP exchange, so the factory only has to satisfy the type.
class AbiDriverGatewayValidateTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NO_THROW(catalog_.load_driver(std::string(kPluginPath)));
        ASSERT_NO_THROW(catalog_.load_driver(std::string(kNoValidatePluginPath)));
        gateway_ = std::make_unique<AbiDriverGateway>(
            catalog_, []() -> std::unique_ptr<HttpClient> { return std::make_unique<QueueHttpClient>(); },
            logger_);
    }

    DriverCatalog catalog_;
    RecordingLogger logger_;
    std::unique_ptr<AbiDriverGateway> gateway_;
};

}  // namespace

TEST_F(AbiDriverGatewayValidateTest, ValidDriverParamSucceeds) {
    const auto result = gateway_->validate_config(kDriverName, R"({"op":"success"})");
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(AbiDriverGatewayValidateTest, DriverRejectionMapsPluginMessageVerbatim) {
    const auto result = gateway_->validate_config(kDriverName, R"({"op":"reject_validate","message":"bad zone_id"})");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_EQ(result.error().message, "bad zone_id");
}

TEST_F(AbiDriverGatewayValidateTest, DriverRejectionEmptyMessageFallsBackToWording) {
    const auto result = gateway_->validate_config(kDriverName, R"({"op":"reject_validate","message":""})");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_EQ(result.error().message, "Driver 'test_driver_plugin' rejected its driver_param configuration");
}

TEST_F(AbiDriverGatewayValidateTest, PluginWithoutValidateEntryIsSkipped) {
    // The OPTIONAL entry is absent: the host must skip the driver-side check
    // and report success instead of failing.
    const auto result = gateway_->validate_config(kNoValidateDriverName, R"({"anything":true})");
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST_F(AbiDriverGatewayValidateTest, UnknownDriverReportsNotFound) {
    const auto result = gateway_->validate_config("missing", R"({"op":"success"})");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::NOT_FOUND);
    EXPECT_EQ(result.error().message, "Driver 'missing' is not loaded");
}
