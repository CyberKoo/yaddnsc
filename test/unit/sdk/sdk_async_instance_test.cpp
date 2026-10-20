//
// Async instance failure-path tests.
//
// The async instance is the seam where a host event reaches plugin business
// logic: it validates the event shape before it resumes the driver's
// coroutine and it translates whatever the plugin throws into an ABI status.
// A malformed event that slips past those checks would run plugin code with
// garbage, so these cases pin the rejections, the exception translation and
// the one-shot contract.
//

#include <coroutine>
#include <cstdint>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>

namespace sdk = yaddnsc::sdk;

namespace {

/// What the fake host answers to http_start.
struct FakeHost {
    yaddnsc_status start_status = YADDNSC_STATUS_OK;
    uint64_t next_op_id = 1;
    int start_calls = 0;
};

void fake_log(void*, yaddnsc_log_level, yaddnsc_string, const yaddnsc_source_location*) {}

int32_t fake_log_enabled(void*, yaddnsc_log_level) {
    return 0;
}

int32_t fake_cancelled(void*) {
    return 0;
}

yaddnsc_status fake_start(void* context, const yaddnsc_http_request*, uint64_t* out_op_id, yaddnsc_error* out_error) {
    auto* host = static_cast<FakeHost*>(context);
    ++host->start_calls;
    if (host->start_status != YADDNSC_STATUS_OK) {
        if (out_error != nullptr) {
            static constexpr std::string_view message{"host refused the request"};
            out_error->status = host->start_status;
            out_error->message = {message.data(), message.size()};
        }
        return host->start_status;
    }
    if (out_op_id != nullptr) {
        *out_op_id = host->next_op_id;
    }
    return YADDNSC_STATUS_OK;
}

yaddnsc_status fake_cancel(void*, uint64_t, yaddnsc_error*) {
    return YADDNSC_STATUS_OK;
}

yaddnsc_async_services make_services(FakeHost* host) {
    yaddnsc_async_services services{};
    services.struct_size = static_cast<uint32_t>(sizeof(services));
    services.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    services.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    services.context = host;
    services.log = &fake_log;
    services.is_cancelled = &fake_cancelled;
    services.http_start = &fake_start;
    services.http_cancel = &fake_cancel;
    services.log_enabled = &fake_log_enabled;
    return services;
}

yaddnsc_error make_error() {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    return error;
}

yaddnsc_update_request make_request() {
    static constexpr std::string_view ip{"192.0.2.1"};
    static constexpr std::string_view type{"A"};
    static constexpr std::string_view domain{"example.com"};
    static constexpr std::string_view subdomain{"www"};
    static constexpr std::string_view fqdn{"www.example.com"};
    static constexpr std::string_view params{"{}"};

    yaddnsc_update_request request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.ip_address = {ip.data(), ip.size()};
    request.record_type = {type.data(), type.size()};
    request.domain = {domain.data(), domain.size()};
    request.subdomain = {subdomain.data(), subdomain.size()};
    request.fqdn = {fqdn.data(), fqdn.size()};
    request.driver_param_json = {reinterpret_cast<const uint8_t*>(params.data()), params.size()};
    return request;
}

yaddnsc_http_response make_response(uint32_t status_code) {
    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    response.status_code = status_code;
    return response;
}

yaddnsc_event make_event(yaddnsc_event_kind kind, uint64_t op_id, yaddnsc_status status) {
    yaddnsc_event event{};
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    event.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    event.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    event.kind = kind;
    event.op_id = op_id;
    event.status = status;
    return event;
}

std::string message_of(const yaddnsc_error& error) {
    return std::string(sdk::detail::to_view(error.message));
}

/// The ABI create entry point, wrapped so that the template arguments do not
/// split the assertion macros at their comma.
template<typename Driver>
yaddnsc_status create_driver(const yaddnsc_async_services* services, yaddnsc_async_driver** out_driver,
                             yaddnsc_error* out_error) {
    return sdk::detail::create_async_driver<Driver, 1>(services, out_driver, out_error);
}

/// The driver the tests observe: it awaits one exchange and reports what it
/// received, so a delivered event can be told apart from a dropped one.
uint32_t g_observed_status = 0;
std::string g_observed_body;
yaddnsc_status g_observed_error_status = YADDNSC_STATUS_OK;

class ExchangingDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext& context) override {
        auto response = co_await context.exchange(sdk::HttpRequest{});
        if (!response) {
            g_observed_error_status = response.error().status;
            co_return std::unexpected(sdk::Error{response.error().status, response.error().message, 0});
        }
        g_observed_status = response->status_code;
        g_observed_body = response->body;
        co_return {};
    }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }
};

/// A driver that never touches the network: the update completes at once.
class ImmediateDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext&) override { co_return {}; }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }
};

/// A driver whose update throws before it returns a coroutine, so the throw
/// reaches the caller of update() instead of the coroutine body.
class ThrowingEntryDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext&) override {
        throw sdk::ConfigParseError("driver configuration is unusable");
    }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }
};

/// What the update coroutine throws once the instance resumes it.
enum class TaskFailure { None, Config, Standard, Unknown };

TaskFailure g_task_failure = TaskFailure::None;

/// A driver whose coroutine body throws once the instance resumes it.
class ThrowingTaskDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext&) override {
        switch (g_task_failure) {
            case TaskFailure::Config:
                throw sdk::ConfigParseError("driver configuration is unusable");
            case TaskFailure::Standard:
                throw std::runtime_error("the update failed");
            case TaskFailure::Unknown:
                throw 42;
            case TaskFailure::None:
                break;
        }
        co_return {};
    }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }
};

/// A driver that suspends without an exchange, which no host event can wake.
class SuspendingDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext&) override {
        co_await std::suspend_always{};
        co_return {};
    }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }
};

/// A driver whose validate entry throws each kind of failure the ABI entry
/// has to translate.
enum class ValidateFailure { None, Config, Standard, Unknown };

ValidateFailure g_validate_failure = ValidateFailure::None;

class ThrowingValidateDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext&) override { co_return {}; }

    [[nodiscard]] sdk::Result validate(std::string_view) const override {
        switch (g_validate_failure) {
            case ValidateFailure::Config:
                throw sdk::ConfigParseError("driver configuration is unusable");
            case ValidateFailure::Standard:
                throw std::runtime_error("validate failed");
            case ValidateFailure::Unknown:
                throw 42;
            case ValidateFailure::None:
                break;
        }
        return {};
    }
};

/// A driver that reports a failed validation result instead of throwing.
class RejectingValidateDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext&) override { co_return {}; }

    [[nodiscard]] sdk::Result validate(std::string_view) const override {
        return std::unexpected(sdk::Error{YADDNSC_STATUS_INVALID_CONFIG, "the document is not acceptable", 0});
    }
};

/// A driver that cannot be constructed, so the async create entry has to
/// report the failure instead of handing out an instance.
class ThrowingConstructorDriver final : public sdk::AsyncDriver {
public:
    explicit ThrowingConstructorDriver(sdk::AsyncServices) { throw std::runtime_error("construction failed"); }

    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext&) override { co_return {}; }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }
};

}  // namespace

TEST(SdkAsyncInstanceCreate, RejectsNullArguments) {
    yaddnsc_async_services services = make_services(nullptr);
    yaddnsc_async_driver* driver = nullptr;
    yaddnsc_error error = make_error();

    EXPECT_EQ(create_driver<ImmediateDriver>(nullptr, &driver, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "services and out_driver must not be null");

    EXPECT_EQ(create_driver<ImmediateDriver>(&services, nullptr, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "services and out_driver must not be null");
}

TEST(SdkAsyncInstanceCreate, RejectsAnIncompatibleServicesTable) {
    FakeHost host;
    yaddnsc_error error = make_error();

    {
        yaddnsc_async_services services = make_services(&host);
        services.struct_size = YADDNSC_ASYNC_SERVICES_MIN_SIZE - 1;
        yaddnsc_async_driver* driver = nullptr;
        EXPECT_EQ(create_driver<ImmediateDriver>(&services, &driver, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "incompatible async host services table");
    }
    {
        yaddnsc_async_services services = make_services(&host);
        services.abi_major = YADDNSC_DRIVER_ABI_MAJOR + 1;
        yaddnsc_async_driver* driver = nullptr;
        EXPECT_EQ(create_driver<ImmediateDriver>(&services, &driver, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
    {
        yaddnsc_async_services services = make_services(&host);
        services.abi_minor = 0;
        yaddnsc_async_driver* driver = nullptr;
        EXPECT_EQ(create_driver<ImmediateDriver>(&services, &driver, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
    {
        yaddnsc_async_services services = make_services(&host);
        services.context = nullptr;
        yaddnsc_async_driver* driver = nullptr;
        EXPECT_EQ(create_driver<ImmediateDriver>(&services, &driver, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
    {
        yaddnsc_async_services services = make_services(&host);
        services.log = nullptr;
        yaddnsc_async_driver* driver = nullptr;
        EXPECT_EQ(create_driver<ImmediateDriver>(&services, &driver, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
    {
        yaddnsc_async_services services = make_services(&host);
        services.is_cancelled = nullptr;
        yaddnsc_async_driver* driver = nullptr;
        EXPECT_EQ(create_driver<ImmediateDriver>(&services, &driver, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
    {
        yaddnsc_async_services services = make_services(&host);
        services.http_start = nullptr;
        yaddnsc_async_driver* driver = nullptr;
        EXPECT_EQ(create_driver<ImmediateDriver>(&services, &driver, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
    {
        yaddnsc_async_services services = make_services(&host);
        services.http_cancel = nullptr;
        yaddnsc_async_driver* driver = nullptr;
        EXPECT_EQ(create_driver<ImmediateDriver>(&services, &driver, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    }
}

TEST(SdkAsyncInstanceCreate, AcceptsACompleteServicesTable) {
    FakeHost host;
    auto services = make_services(&host);
    yaddnsc_async_driver* driver = nullptr;
    yaddnsc_error error = make_error();

    EXPECT_EQ(create_driver<ImmediateDriver>(&services, &driver, &error), YADDNSC_STATUS_OK);
    EXPECT_NE(driver, nullptr);
    delete reinterpret_cast<sdk::detail::AsyncInstance<ImmediateDriver>*>(driver);  // NOLINT
}

TEST(SdkAsyncInstanceCreate, ReportsAThrowingDriverConstructor) {
    FakeHost host;
    auto services = make_services(&host);
    yaddnsc_async_driver* driver = nullptr;
    yaddnsc_error error = make_error();

    EXPECT_EQ(create_driver<ThrowingConstructorDriver>(&services, &driver, &error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(message_of(error), "construction failed");
    EXPECT_EQ(driver, nullptr);
}

TEST(SdkAsyncInstanceBegin, RejectsAnInvalidUpdateRequest) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ImmediateDriver> instance(&services);
    yaddnsc_error error = make_error();

    auto request = make_request();
    request.struct_size = YADDNSC_UPDATE_REQUEST_MIN_SIZE - 1;
    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid async update request");

    // The rejected request consumes the instance.
    auto valid = make_request();
    EXPECT_EQ(instance.begin(valid, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "async instance already used");
}

TEST(SdkAsyncInstanceBegin, RejectsASecondCall) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ImmediateDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();

    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "async instance already used");
}

TEST(SdkAsyncInstanceBegin, ReportsAThrowingDriverEntry) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ThrowingEntryDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();

    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_EQ(message_of(error), "driver configuration is unusable");
}

TEST(SdkAsyncInstanceBegin, ReportsAConfigurationErrorFromTheTask) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ThrowingTaskDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    g_task_failure = TaskFailure::Config;

    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_EQ(message_of(error), "driver configuration is unusable");
    g_task_failure = TaskFailure::None;
}

TEST(SdkAsyncInstanceBegin, ReportsAStandardFailureFromTheTask) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ThrowingTaskDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    g_task_failure = TaskFailure::Standard;

    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(message_of(error), "the update failed");
    g_task_failure = TaskFailure::None;
}

TEST(SdkAsyncInstanceBegin, ReportsAnUnknownFailureFromTheTask) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ThrowingTaskDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    g_task_failure = TaskFailure::Unknown;

    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(message_of(error), "async task failed");
    g_task_failure = TaskFailure::None;
}

TEST(SdkAsyncInstanceBegin, ReportsATaskThatSuspendedWithoutAnExchange) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<SuspendingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();

    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(message_of(error), "async task suspended without a pending exchange");
}

TEST(SdkAsyncInstanceResume, RejectsAnEventBeforeTheUpdateStarted) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_OK);

    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "async instance is not waiting for an event");
}

TEST(SdkAsyncInstanceResume, RejectsAMalformedEvent) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    {
        auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_OK);
        event.struct_size = YADDNSC_EVENT_MIN_SIZE - 1;
        EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async event");
    }
    {
        auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_OK);
        event.abi_major = YADDNSC_DRIVER_ABI_MAJOR + 1;
        EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async event");
    }
    {
        auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_OK);
        event.abi_minor = 0;
        EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async event");
    }
}

TEST(SdkAsyncInstanceResume, RejectsAnUnknownEventKind) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    auto event = make_event(99, 1, YADDNSC_STATUS_OK);
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "unknown async event kind");
}

TEST(SdkAsyncInstanceResume, RejectsACompletionEventForAnotherOperation) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 42, YADDNSC_STATUS_OK);
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid async completion event");
}

TEST(SdkAsyncInstanceResume, RejectsACompletionEventThatIsStillPending) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_PENDING);
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid async completion event");
}

TEST(SdkAsyncInstanceResume, RejectsAnUnknownCompletionStatus) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, 4242);
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid async completion event");
}

TEST(SdkAsyncInstanceResume, RejectsAResponseEventWithoutAResponse) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_OK);
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid async response event");
}

TEST(SdkAsyncInstanceResume, RejectsAResponseEventThatCarriesAnError) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    auto response = make_response(200);
    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_OK);
    event.response = &response;
    event.error = &error;
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid async response event");
}

TEST(SdkAsyncInstanceResume, RejectsAResponseEventBelowTheBaseline) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    auto response = make_response(200);
    response.struct_size = YADDNSC_HTTP_RESPONSE_MIN_SIZE - 1;
    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_OK);
    event.response = &response;
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid async response event");
}

TEST(SdkAsyncInstanceResume, RejectsAnErrorEventWithoutAnError) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid async error event");
}

TEST(SdkAsyncInstanceResume, RejectsAnErrorEventWithAMismatchedStatus) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    yaddnsc_error payload = make_error();
    payload.status = YADDNSC_STATUS_UPSTREAM_REJECTED;
    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_NETWORK_ERROR);
    event.error = &payload;
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid async error event");
}

TEST(SdkAsyncInstanceResume, DeliversTheResponseToTheDriver) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    static constexpr std::string_view body{"payload"};
    yaddnsc_http_response response = make_response(201);
    response.body = {reinterpret_cast<const uint8_t*>(body.data()), body.size()};

    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_OK);
    event.response = &response;
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(g_observed_status, 201u);
    EXPECT_EQ(g_observed_body, "payload");
}

TEST(SdkAsyncInstanceResume, DeliversTheErrorToTheDriver) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    static constexpr std::string_view message{"the upstream refused the request"};
    yaddnsc_error payload = make_error();
    payload.status = YADDNSC_STATUS_UPSTREAM_REJECTED;
    payload.message = {message.data(), message.size()};

    auto event = make_event(YADDNSC_EVENT_HTTP_FINISHED, 1, YADDNSC_STATUS_UPSTREAM_REJECTED);
    event.error = &payload;
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_UPSTREAM_REJECTED);
    EXPECT_EQ(g_observed_error_status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(SdkAsyncInstanceResume, RejectsACancellationEventWithTheWrongShape) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    static constexpr std::string_view message{"update cancelled"};
    yaddnsc_error payload = make_error();
    payload.status = YADDNSC_STATUS_CANCELLED;
    payload.message = {message.data(), message.size()};

    {
        auto event = make_event(YADDNSC_EVENT_UPDATE_CANCELLED, 1, YADDNSC_STATUS_CANCELLED);
        event.error = &payload;
        EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async cancellation event");
    }
    {
        auto event = make_event(YADDNSC_EVENT_UPDATE_CANCELLED, 0, YADDNSC_STATUS_CANCELLED);
        event.error = &payload;
        payload.status = YADDNSC_STATUS_INTERNAL_ERROR;
        EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async cancellation event");
    }
}

TEST(SdkAsyncInstanceValidate, RejectsAnInvalidParameterView) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ImmediateDriver> instance(&services);
    yaddnsc_error error = make_error();

    EXPECT_EQ(instance.validate(yaddnsc_string{nullptr, 2}, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "invalid driver_param_json view");
}

TEST(SdkAsyncInstanceValidate, TranslatesAFailedResult) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<RejectingValidateDriver> instance(&services);
    yaddnsc_error error = make_error();
    const yaddnsc_string json{"{}", 2};

    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_EQ(message_of(error), "the document is not acceptable");
}

TEST(SdkAsyncInstanceValidate, ReportsAConfigurationError) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ThrowingValidateDriver> instance(&services);
    yaddnsc_error error = make_error();
    const yaddnsc_string json{"{}", 2};
    g_validate_failure = ValidateFailure::Config;

    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_EQ(message_of(error), "driver configuration is unusable");
    g_validate_failure = ValidateFailure::None;
}

TEST(SdkAsyncInstanceValidate, ReportsAStandardFailure) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ThrowingValidateDriver> instance(&services);
    yaddnsc_error error = make_error();
    const yaddnsc_string json{"{}", 2};
    g_validate_failure = ValidateFailure::Standard;

    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(message_of(error), "validate failed");
    g_validate_failure = ValidateFailure::None;
}

TEST(SdkAsyncInstanceValidate, ReportsAnUnknownFailure) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ThrowingValidateDriver> instance(&services);
    yaddnsc_error error = make_error();
    const yaddnsc_string json{"{}", 2};
    g_validate_failure = ValidateFailure::Unknown;

    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(message_of(error), "unknown exception during async validate");
    g_validate_failure = ValidateFailure::None;
}

TEST(SdkAsyncInstanceValidate, RejectsASecondCall) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ImmediateDriver> instance(&services);
    yaddnsc_error error = make_error();
    const yaddnsc_string json{"{}", 2};

    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "async instance already used");
}

TEST(SdkAsyncInstanceExchange, RejectsASecondPendingExchange) {
    FakeHost host;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);

    sdk::ExchangeAwaitable first(instance, sdk::HttpRequest{});
    sdk::ExchangeAwaitable second(instance, sdk::HttpRequest{});
    EXPECT_TRUE(instance.start_exchange(first, std::noop_coroutine()));
    EXPECT_FALSE(instance.start_exchange(second, std::noop_coroutine()));

    auto result = second.await_resume();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(result.error().message, "another async exchange is already pending");
}

TEST(SdkAsyncInstanceExchange, ReportsAHostFailure) {
    FakeHost host;
    host.start_status = YADDNSC_STATUS_NETWORK_ERROR;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);

    sdk::ExchangeAwaitable awaitable(instance, sdk::HttpRequest{});
    EXPECT_FALSE(instance.start_exchange(awaitable, std::noop_coroutine()));

    auto result = awaitable.await_resume();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(result.error().message, "host refused the request");
}

TEST(SdkAsyncInstanceExchange, RejectsAZeroOperationId) {
    FakeHost host;
    host.next_op_id = 0;
    auto services = make_services(&host);
    sdk::detail::AsyncInstance<ExchangingDriver> instance(&services);

    sdk::ExchangeAwaitable awaitable(instance, sdk::HttpRequest{});
    EXPECT_FALSE(instance.start_exchange(awaitable, std::noop_coroutine()));

    auto result = awaitable.await_resume();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(result.error().message, "host returned a zero async op id");
}
