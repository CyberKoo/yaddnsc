/// Contract tests for the ABI 1.1 async entry points and PENDING/event
/// hand-off. The fixture is a pure C ABI module with no SDK and no sync entry
/// points; the test supplies the async services table and drives the state
/// machine directly.
///
/// This validates the interface itself (declarations, status sets, event
/// payloads, ownership and PENDING/resume). The production loader/gateway and
/// the async SDK are covered by their own tests.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "infrastructure/plugin/plugin_loader.h"
#include "infrastructure/plugin/shared_library.h"

namespace {

constexpr std::string_view PLUGIN_PATH = TEST_ASYNC_PLUGIN_PATH;

struct AsyncHost {
    yaddnsc_async_services services{};
    int start_calls = 0;
    int log_calls = 0;
    uint64_t last_op_id = 0;
    uint64_t next_op_id = 1;
    std::string last_url;
    bool cancelled = false;
    bool fail_start = false;
    yaddnsc_status start_status = YADDNSC_STATUS_OK;
    yaddnsc_status cancel_status = YADDNSC_STATUS_OK;

    AsyncHost() {
        services.struct_size = static_cast<uint32_t>(sizeof(yaddnsc_async_services));
        services.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
        services.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
        services.context = this;
        services.log = &log_entry;
        services.is_cancelled = &is_cancelled_entry;
        services.http_start = &http_start_entry;
        services.http_cancel = &http_cancel_entry;
        services.log_enabled = &log_enabled_entry;
    }

    static void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message) noexcept {
        if (out_error == nullptr || out_error->struct_size < YADDNSC_ERROR_MIN_SIZE) {
            return;
        }
        out_error->status = status;
        out_error->retry_after_seconds = 0;
        out_error->message = {message.data(), message.size()};
        out_error->struct_size = out_error->struct_size < static_cast<uint32_t>(sizeof(yaddnsc_error))
                                     ? out_error->struct_size
                                     : static_cast<uint32_t>(sizeof(yaddnsc_error));
    }

    static yaddnsc_status http_start_entry(void* context, const yaddnsc_http_request* request,
                                           uint64_t* out_op_id, yaddnsc_error* out_error) {
        auto& self = *static_cast<AsyncHost*>(context);
        ++self.start_calls;
        if (request == nullptr || out_op_id == nullptr || request->struct_size < YADDNSC_HTTP_REQUEST_MIN_SIZE ||
            !yaddnsc_string_is_valid(request->url) || request->url.size == 0) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "bad async request");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        try {
            self.last_url.assign(request->url.data, request->url.size);
        } catch (...) {
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "host allocation failure");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
        *out_op_id = 0;
        if (self.fail_start) {
            write_error(out_error, self.start_status, "scripted start failure");
            return self.start_status;
        }
        *out_op_id = self.next_op_id++;
        self.last_op_id = *out_op_id;
        return YADDNSC_STATUS_OK;
    }

    static yaddnsc_status http_cancel_entry(void* context, uint64_t op_id, yaddnsc_error* out_error) {
        auto& self = *static_cast<AsyncHost*>(context);
        if (op_id == 0 || op_id != self.last_op_id) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "bad async op id");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        if (self.cancel_status != YADDNSC_STATUS_OK) {
            write_error(out_error, self.cancel_status, "scripted cancel failure");
            return self.cancel_status;
        }
        return YADDNSC_STATUS_OK;
    }

    static int32_t is_cancelled_entry(void* context) {
        return static_cast<AsyncHost*>(context)->cancelled ? 1 : 0;
    }

    static void log_entry(void* context, yaddnsc_log_level, yaddnsc_string, const yaddnsc_source_location*) {
        ++static_cast<AsyncHost*>(context)->log_calls;
    }

    static int32_t log_enabled_entry(void*, yaddnsc_log_level) { return 1; }
};

struct AsyncFixture {
    SharedLibrary library;
    decltype(&yaddnsc_driver_get_descriptor) get_descriptor = nullptr;
    decltype(&yaddnsc_driver_async_create) create = nullptr;
    decltype(&yaddnsc_driver_async_destroy) destroy = nullptr;
    decltype(&yaddnsc_driver_update_async_begin) begin = nullptr;
    decltype(&yaddnsc_driver_update_async_resume) resume = nullptr;
    decltype(&yaddnsc_driver_async_validate) validate = nullptr;
};

[[nodiscard]] AsyncFixture load_fixture() {
    AsyncFixture fixture;
    auto library = SharedLibrary::open(std::string(PLUGIN_PATH));
    EXPECT_TRUE(library.has_value()) << library.error();
    if (!library) {
        return fixture;
    }
    fixture.library = std::move(*library);
    fixture.get_descriptor = reinterpret_cast<decltype(fixture.get_descriptor)>(
        fixture.library.resolve("yaddnsc_driver_get_descriptor"));  // NOLINT
    fixture.create = reinterpret_cast<decltype(fixture.create)>(
        fixture.library.resolve("yaddnsc_driver_async_create"));  // NOLINT
    fixture.destroy = reinterpret_cast<decltype(fixture.destroy)>(
        fixture.library.resolve("yaddnsc_driver_async_destroy"));  // NOLINT
    fixture.begin = reinterpret_cast<decltype(fixture.begin)>(
        fixture.library.resolve("yaddnsc_driver_update_async_begin"));  // NOLINT
    fixture.resume = reinterpret_cast<decltype(fixture.resume)>(
        fixture.library.resolve("yaddnsc_driver_update_async_resume"));  // NOLINT
    fixture.validate = reinterpret_cast<decltype(fixture.validate)>(
        fixture.library.resolve("yaddnsc_driver_async_validate"));  // NOLINT
    return fixture;
}

[[nodiscard]] yaddnsc_error make_error() {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    return error;
}

[[nodiscard]] yaddnsc_update_request make_update_request() {
    yaddnsc_update_request request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.ip_address = {"192.0.2.1", sizeof("192.0.2.1") - 1};
    request.record_type = {"A", sizeof("A") - 1};
    request.domain = {"example.com", sizeof("example.com") - 1};
    request.subdomain = {"www", sizeof("www") - 1};
    request.fqdn = {"www.example.com", sizeof("www.example.com") - 1};
    request.driver_param_json = {reinterpret_cast<const uint8_t*>("{}"), sizeof("{}") - 1};
    return request;
}

[[nodiscard]] yaddnsc_http_response make_response() {
    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    response.status_code = 204;
    response.headers = nullptr;
    response.header_count = 0;
    response.body = {nullptr, 0};
    return response;
}

[[nodiscard]] yaddnsc_event make_finished_event(uint64_t op_id, const yaddnsc_http_response* response) {
    yaddnsc_event event{};
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    event.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    event.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    event.kind = YADDNSC_EVENT_HTTP_FINISHED;
    event.op_id = op_id;
    event.status = YADDNSC_STATUS_OK;
    event.response = response;
    event.error = nullptr;
    return event;
}

[[nodiscard]] yaddnsc_event make_error_event(uint64_t op_id, yaddnsc_status status, const yaddnsc_error* error) {
    yaddnsc_event event{};
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    event.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    event.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    event.kind = YADDNSC_EVENT_HTTP_FINISHED;
    event.op_id = op_id;
    event.status = status;
    event.response = nullptr;
    event.error = error;
    return event;
}

[[nodiscard]] yaddnsc_event make_cancel_event(const yaddnsc_error* error) {
    yaddnsc_event event{};
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    event.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    event.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    event.kind = YADDNSC_EVENT_UPDATE_CANCELLED;
    event.op_id = 0;
    event.status = YADDNSC_STATUS_CANCELLED;
    event.response = nullptr;
    event.error = error;
    return event;
}

/// Owns one async handle and destroys it before the fixture library unloads.
struct AsyncHandle {
    AsyncFixture& fixture;
    yaddnsc_async_driver* handle = nullptr;

    ~AsyncHandle() {
        if (handle != nullptr) {
            fixture.destroy(handle);
        }
    }
};

class AsyncAbiContract : public ::testing::Test {
protected:
    void SetUp() override {
        fixture_ = load_fixture();
        ASSERT_NE(fixture_.create, nullptr);
        ASSERT_NE(fixture_.destroy, nullptr);
        ASSERT_NE(fixture_.begin, nullptr);
        ASSERT_NE(fixture_.resume, nullptr);
        ASSERT_NE(fixture_.validate, nullptr);
    }

    AsyncHost host_;
    AsyncFixture fixture_;
};

TEST(AsyncLoader, SyncMinorOneWithoutAsyncSymbolsStaysSync) {
    auto module = PluginModule::load(std::string(SYNC_MINOR_ONE_FIXTURE));
    ASSERT_TRUE(module.has_value()) << module.error().message;
    EXPECT_EQ(module->mode(), ExecutionMode::Sync);
}

TEST(AsyncLoader, RejectsMixedGroups) {
    auto module = PluginModule::load(std::string(ASYNC_MIXED_FIXTURE));
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::CONTRACT_VIOLATION);
    EXPECT_NE(module.error().message.find("mixes"), std::string::npos);
}

TEST(AsyncLoader, RejectsIncompleteAsyncGroup) {
    auto module = PluginModule::load(std::string(ASYNC_INCOMPLETE_FIXTURE));
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::MISSING_SYMBOL);
    EXPECT_NE(module.error().message.find("complete async execution group"), std::string::npos);
}

TEST(AsyncLoader, RejectsAsyncGroupBelowMinorOne) {
    auto module = PluginModule::load(std::string(ASYNC_OLD_MINOR_FIXTURE));
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::ABI_MISMATCH);
    EXPECT_NE(module.error().message.find("async mode requires"), std::string::npos);
}

TEST(AsyncLoader, RejectsAsyncGroupWithSyncValidate) {
    auto module = PluginModule::load(std::string(ASYNC_WITH_SYNC_VALIDATE_FIXTURE));
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::CONTRACT_VIOLATION);
    EXPECT_NE(module.error().message.find("mixes"), std::string::npos);
}

TEST(AsyncLoader, LegacySyncWithExtraAsyncNamesStaysSync) {
    auto module = PluginModule::load(std::string(SYNC_LEGACY_EXTRA_ASYNC_FIXTURE));
    ASSERT_TRUE(module.has_value()) << module.error().message;
    EXPECT_EQ(module->mode(), ExecutionMode::Sync);
    EXPECT_TRUE(module->supports_validate());
}

TEST(AsyncLoader, SelectsAsyncModeAndDrivesTheGroup) {
    auto module = PluginModule::load(std::string(PLUGIN_PATH));
    ASSERT_TRUE(module.has_value()) << module.error().message;
    EXPECT_EQ(module->mode(), ExecutionMode::Async);
    EXPECT_TRUE(module->supports_validate());

    AsyncHost host;
    yaddnsc_error error = make_error();
    yaddnsc_async_driver* handle = nullptr;
    ASSERT_EQ(module->async_create(host.services, &handle, error), YADDNSC_STATUS_OK);
    ASSERT_NE(handle, nullptr);

    const auto request = make_update_request();
    ASSERT_EQ(module->async_begin(handle, request, error), YADDNSC_STATUS_PENDING);
    const auto response = make_response();
    const auto event = make_finished_event(host.last_op_id, &response);
    ASSERT_EQ(module->async_resume(handle, event, error), YADDNSC_STATUS_OK);
    module->async_destroy(handle);
}

TEST_F(AsyncAbiContract, DescriptorIsCommonAndTheFixtureIsAsyncOnly) {
    const yaddnsc_driver_descriptor* descriptor = nullptr;
    ASSERT_EQ(fixture_.get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->struct_size, sizeof(yaddnsc_driver_descriptor));
    EXPECT_EQ(descriptor->abi_major, YADDNSC_DRIVER_ABI_MAJOR);
    EXPECT_EQ(descriptor->abi_minor, YADDNSC_DRIVER_ABI_MINOR);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);

    EXPECT_EQ(fixture_.library.resolve("yaddnsc_driver_create"), nullptr);
    EXPECT_EQ(fixture_.library.resolve("yaddnsc_driver_update"), nullptr);
}

TEST_F(AsyncAbiContract, AsyncCreateRejectsMalformedServicesAndOwnsTheHandle) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();

    EXPECT_EQ(fixture_.create(nullptr, &driver.handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(driver.handle, nullptr);

    EXPECT_EQ(fixture_.create(&host_.services, nullptr, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    auto bad = host_.services;
    bad.struct_size = YADDNSC_ASYNC_SERVICES_MIN_SIZE - 1;
    EXPECT_EQ(fixture_.create(&bad, &driver.handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(driver.handle, nullptr);

    bad = host_.services;
    bad.abi_major = static_cast<uint16_t>(YADDNSC_DRIVER_ABI_MAJOR + 1);
    EXPECT_EQ(fixture_.create(&bad, &driver.handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(driver.handle, nullptr);

    bad = host_.services;
    bad.http_start = nullptr;
    EXPECT_EQ(fixture_.create(&bad, &driver.handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(driver.handle, nullptr);

    EXPECT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);
    ASSERT_NE(driver.handle, nullptr);
    fixture_.destroy(nullptr);
}

TEST_F(AsyncAbiContract, BeginReturnsPendingThenResumeCompletes) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    const auto request = make_update_request();
    ASSERT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(host_.start_calls, 1);
    EXPECT_EQ(host_.last_url, "http://127.0.0.1/async");
    ASSERT_NE(host_.last_op_id, 0u);

    const auto response = make_response();
    const auto event = make_finished_event(host_.last_op_id, &response);
    EXPECT_EQ(fixture_.resume(driver.handle, &event, &error), YADDNSC_STATUS_OK);
}

TEST_F(AsyncAbiContract, ResumePropagatesEventError) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    const auto request = make_update_request();
    ASSERT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_PENDING);

    yaddnsc_error event_error = make_error();
    event_error.status = YADDNSC_STATUS_NETWORK_ERROR;
    event_error.message = {"upstream failed", sizeof("upstream failed") - 1};
    const auto event = make_error_event(host_.last_op_id, YADDNSC_STATUS_NETWORK_ERROR, &event_error);

    yaddnsc_error out_error = make_error();
    EXPECT_EQ(fixture_.resume(driver.handle, &event, &out_error), YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(out_error.status, YADDNSC_STATUS_NETWORK_ERROR);

    const yaddnsc_string valid{"{}", sizeof("{}") - 1};
    EXPECT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(fixture_.validate(driver.handle, valid, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
}

TEST_F(AsyncAbiContract, ResumeHandlesTheCancellationEvent) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    const auto request = make_update_request();
    ASSERT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_PENDING);

    yaddnsc_error cancel_error = make_error();
    cancel_error.status = YADDNSC_STATUS_CANCELLED;
    cancel_error.message = {"cancel", sizeof("cancel") - 1};
    const auto event = make_cancel_event(&cancel_error);

    yaddnsc_error out_error = make_error();
    EXPECT_EQ(fixture_.resume(driver.handle, &event, &out_error), YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(out_error.status, YADDNSC_STATUS_CANCELLED);

    EXPECT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(fixture_.resume(driver.handle, &event, &out_error), YADDNSC_STATUS_INVALID_ARGUMENT);
}

TEST_F(AsyncAbiContract, BeginHonoursCancellationBeforeHttpStart) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    host_.cancelled = true;
    const auto request = make_update_request();
    EXPECT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(host_.start_calls, 0);
    EXPECT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
}

TEST_F(AsyncAbiContract, HttpStartFailureTerminalizesTheInstance) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    host_.fail_start = true;
    host_.start_status = YADDNSC_STATUS_RATE_LIMITED;
    const auto request = make_update_request();
    EXPECT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_RATE_LIMITED);
    EXPECT_EQ(host_.start_calls, 1);

    // A terminal error does not leave this instance retryable; a new cycle
    // gets a new instance.
    EXPECT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(host_.start_calls, 1);

    AsyncHandle retry{fixture_};
    host_.fail_start = false;
    ASSERT_EQ(fixture_.create(&host_.services, &retry.handle, &error), YADDNSC_STATUS_OK);
    ASSERT_EQ(fixture_.begin(retry.handle, &request, &error), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(host_.start_calls, 2);

    const auto response = make_response();
    const auto event = make_finished_event(host_.last_op_id, &response);
    EXPECT_EQ(fixture_.resume(retry.handle, &event, &error), YADDNSC_STATUS_OK);
}

TEST_F(AsyncAbiContract, InvalidResumeKeepsThePendingOp) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    const auto request = make_update_request();
    ASSERT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_PENDING);

    EXPECT_EQ(fixture_.resume(driver.handle, nullptr, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    auto malformed = make_finished_event(host_.last_op_id, nullptr);
    malformed.struct_size = YADDNSC_EVENT_MIN_SIZE - 1;
    EXPECT_EQ(fixture_.resume(driver.handle, &malformed, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    auto unknown_kind = make_finished_event(host_.last_op_id, nullptr);
    unknown_kind.kind = UINT32_C(99);
    EXPECT_EQ(fixture_.resume(driver.handle, &unknown_kind, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    auto wrong_op = make_finished_event(host_.last_op_id + 1, nullptr);
    EXPECT_EQ(fixture_.resume(driver.handle, &wrong_op, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    auto missing_response = make_finished_event(host_.last_op_id, nullptr);
    EXPECT_EQ(fixture_.resume(driver.handle, &missing_response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    // The rejected events must not consume the accepted op.
    const auto response = make_response();
    const auto event = make_finished_event(host_.last_op_id, &response);
    EXPECT_EQ(fixture_.resume(driver.handle, &event, &error), YADDNSC_STATUS_OK);
}

TEST_F(AsyncAbiContract, BeginRejectsMalformedRequest) {
    yaddnsc_error error = make_error();
    const auto request = make_update_request();

    {
        AsyncHandle driver{fixture_};
        ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);
        auto malformed = request;
        malformed.struct_size = 0;
        EXPECT_EQ(fixture_.begin(driver.handle, &malformed, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
    {
        AsyncHandle driver{fixture_};
        ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);
        auto malformed = request;
        malformed.ip_address = {nullptr, 1};
        EXPECT_EQ(fixture_.begin(driver.handle, &malformed, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
    EXPECT_EQ(host_.start_calls, 0);
}

TEST_F(AsyncAbiContract, UpdateTerminalRejectsAnotherBeginAndResume) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    const auto request = make_update_request();
    ASSERT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_PENDING);
    const auto response = make_response();
    const auto event = make_finished_event(host_.last_op_id, &response);
    ASSERT_EQ(fixture_.resume(driver.handle, &event, &error), YADDNSC_STATUS_OK);

    EXPECT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(fixture_.resume(driver.handle, &event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
}

TEST_F(AsyncAbiContract, ValidateWhileUpdateIsPendingIsRejected) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    const auto request = make_update_request();
    ASSERT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_PENDING);
    const yaddnsc_string valid{"{}", sizeof("{}") - 1};
    EXPECT_EQ(fixture_.validate(driver.handle, valid, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    const auto response = make_response();
    const auto event = make_finished_event(host_.last_op_id, &response);
    ASSERT_EQ(fixture_.resume(driver.handle, &event, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(fixture_.validate(driver.handle, valid, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
}

TEST_F(AsyncAbiContract, BeginAfterValidateIsRejected) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    const yaddnsc_string valid{"{}", sizeof("{}") - 1};
    ASSERT_EQ(fixture_.validate(driver.handle, valid, &error), YADDNSC_STATUS_OK);

    const auto request = make_update_request();
    EXPECT_EQ(fixture_.begin(driver.handle, &request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(host_.start_calls, 0);
}

TEST_F(AsyncAbiContract, ResumeBeforeBeginIsRejected) {
    AsyncHandle driver{fixture_};
    yaddnsc_error error = make_error();
    ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);

    const auto response = make_response();
    const auto event = make_finished_event(1, &response);
    EXPECT_EQ(fixture_.resume(driver.handle, &event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
}

TEST_F(AsyncAbiContract, ValidateEntryIsOneShotAndSeparateFromUpdate) {
    yaddnsc_error error = make_error();
    const yaddnsc_string invalid{nullptr, 1};
    const yaddnsc_string empty{};
    const yaddnsc_string valid{"{}", sizeof("{}") - 1};

    {
        AsyncHandle driver{fixture_};
        ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);
        EXPECT_EQ(fixture_.validate(driver.handle, invalid, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(fixture_.validate(driver.handle, valid, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
    {
        AsyncHandle driver{fixture_};
        ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);
        EXPECT_EQ(fixture_.validate(driver.handle, empty, &error), YADDNSC_STATUS_INVALID_CONFIG);
    }
    {
        AsyncHandle driver{fixture_};
        ASSERT_EQ(fixture_.create(&host_.services, &driver.handle, &error), YADDNSC_STATUS_OK);
        EXPECT_EQ(fixture_.validate(driver.handle, valid, &error), YADDNSC_STATUS_OK);
        EXPECT_EQ(fixture_.validate(driver.handle, valid, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
}

}  // namespace
