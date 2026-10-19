//
// Async SDK lifetime and state-machine tests.
//
// The event/awaitable hand-off is easy to get right only for the happy path:
// these cases read a successful response after its awaitable is destroyed,
// enforce the one-shot validate/update contract, and reject malformed
// cancellation payloads before they can advance the coroutine.
//
// The fake host and the async instance under test are the SDK's host-side
// seam, so this file also includes <yaddnsc/sdk/detail/async_instance.hpp>
// explicitly rather than relying on async_driver.hpp's trailing include.

#include <array>
#include <coroutine>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/upstream_failure.hpp>

namespace sdk = yaddnsc::sdk;

namespace {

class FakeExchangeHost final : public sdk::AsyncExchangeHost {
public:
    sdk::ExchangeAwaitable* pending = nullptr;
    std::coroutine_handle<> continuation;

    bool start_exchange(sdk::ExchangeAwaitable& awaitable, std::coroutine_handle<> handle) override {
        pending = &awaitable;
        continuation = handle;
        return true;
    }
};

void fake_log(void*, yaddnsc_log_level, yaddnsc_string, const yaddnsc_source_location*) {}
int32_t fake_log_enabled(void*, yaddnsc_log_level) { return 0; }
int32_t fake_cancelled(void*) { return 0; }

yaddnsc_status fake_start(void*, const yaddnsc_http_request*, uint64_t* out_op_id, yaddnsc_error*) {
    if (out_op_id != nullptr) {
        *out_op_id = 1;
    }
    return YADDNSC_STATUS_OK;
}

yaddnsc_status fake_cancel(void*, uint64_t, yaddnsc_error*) { return YADDNSC_STATUS_OK; }

yaddnsc_async_services make_services() {
    yaddnsc_async_services services{};
    services.struct_size = static_cast<uint32_t>(sizeof(services));
    services.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    services.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    services.context = reinterpret_cast<void*>(0x1);  // NOLINT: callbacks below ignore it
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

yaddnsc_event make_cancel_event(yaddnsc_error& error) {
    static constexpr std::string_view message{"update cancelled"};
    error = yaddnsc_error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    error.status = YADDNSC_STATUS_CANCELLED;
    error.message = {message.data(), message.size()};

    yaddnsc_event event{};
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    event.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    event.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    event.kind = YADDNSC_EVENT_UPDATE_CANCELLED;
    event.op_id = 0;
    event.status = YADDNSC_STATUS_CANCELLED;
    event.error = &error;
    return event;
}

class ImmediateDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext&) override { co_return {}; }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }
};

class PendingDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext& context) override {
        auto operation = context.exchange(sdk::HttpRequest{});
        auto response = co_await operation;
        if (!response) {
            co_return std::unexpected(sdk::Error{response.error().status, response.error().message,
                                                 response.error().retry_after_seconds});
        }
        co_return {};
    }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }
};

}  // namespace

namespace {

sdk::Task<sdk::AsyncExchangeResult> exchange_temporary(sdk::AsyncUpdateContext& context) {
    auto response = co_await context.exchange(sdk::HttpRequest{});
    // A temporary awaitable has already been destroyed at this statement.
    if (response) {
        response->body.push_back('!');
    }
    co_return response;
}

void expect_owned_response(const sdk::AsyncHttpResponse& response, const std::string& body) {
    EXPECT_EQ(response.status_code, 201u);
    EXPECT_EQ(response.body, body);
    ASSERT_EQ(response.headers.size(), 4u);
    EXPECT_EQ(response.headers[0].name, "X-Dup");
    EXPECT_EQ(response.headers[0].value, "one");
    EXPECT_EQ(response.headers[1].name, "X-Dup");
    EXPECT_EQ(response.headers[1].value, std::string("t\0wo", 4));
    EXPECT_EQ(response.headers[2].name, "x-dup");
    EXPECT_EQ(response.headers[2].value, std::string(128, 'v'));
    EXPECT_TRUE(response.headers[3].name.empty());
    EXPECT_TRUE(response.headers[3].value.empty());
}

}  // namespace

TEST(SdkAsyncResponseOwnership, TemporaryAwaitableAndInputDieBeforeResultIsUsed) {
    static_assert(std::is_same_v<decltype(sdk::AsyncHttpResponse::headers), std::vector<sdk::HttpHeader>>);
    static_assert(std::is_same_v<decltype(sdk::AsyncHttpResponse::body), std::string>);
    static_assert(std::is_same_v<sdk::AsyncExchangeResult, std::expected<sdk::AsyncHttpResponse, sdk::HttpError>>);
    static_assert(!std::is_convertible_v<sdk::AsyncHttpResponse, sdk::HttpResponse>);
    for (const auto& body : {std::string{}, std::string("sso"), std::string("a\0b\xff", 4), std::string(1024, 'b')}) {
        SCOPED_TRACE(body.size());
        sdk::AsyncExchangeResult result;
        {
            FakeExchangeHost host;
            const sdk::AsyncServices services(nullptr, &host);
            sdk::AsyncUpdateContext context(sdk::UpdateRequest{}, services);
            auto task = exchange_temporary(context);
            task.resume();
            ASSERT_FALSE(task.done());
            ASSERT_NE(host.pending, nullptr);
            {
                std::string input = body;
                std::string name = "X-Dup";
                std::string lower_name = "x-dup";
                std::string first = "one";
                std::string second("t\0wo", 4);
                std::string long_value(128, 'v');
                const std::array<yaddnsc_http_header, 4> headers{{
                    {{name.data(), name.size()}, {first.data(), first.size()}},
                    {{name.data(), name.size()}, {second.data(), second.size()}},
                    {{lower_name.data(), lower_name.size()}, {long_value.data(), long_value.size()}},
                    {},
                }};
                yaddnsc_http_response response{};
                response.struct_size = static_cast<uint32_t>(sizeof(response));
                response.status_code = 201;
                response.headers = headers.data();
                response.header_count = headers.size();
                response.body = {reinterpret_cast<const uint8_t*>(input.data()), input.size()};
                host.pending->set_response(response);
                input.assign(input.size(), '?');
                name.assign(name.size(), '?');
                first.assign(first.size(), '?');
                second.assign(second.size(), '?');
                long_value.assign(long_value.size(), '?');
            }
            // Both the ABI payload and its strings are gone before business resume.
            host.continuation.resume();
            ASSERT_TRUE(task.done());
            result = task.take_result();
        }
        ASSERT_TRUE(result.has_value());
        expect_owned_response(*result, body + '!');

        sdk::AsyncHttpResponse copied;
        sdk::AsyncHttpResponse moved;
        {
            sdk::AsyncHttpResponse copy(*result);
            result->body.assign("changed");
            result->headers[0].value.assign("changed");
            copied = copy;
            sdk::AsyncHttpResponse move(std::move(copy));
            moved = std::move(move);
        }
        result = std::unexpected(sdk::HttpError{});
        expect_owned_response(copied, body + '!');
        expect_owned_response(moved, body + '!');
        copied.body.assign("changed again");
        copied.headers[0].value.assign("changed again");
        expect_owned_response(moved, body + '!');
    }
}

TEST(SdkAsyncResponseOwnership, EmptyAbiResponseAndErrorOutliveTheirSources) {
    FakeExchangeHost host;
    sdk::AsyncExchangeResult result;
    {
        sdk::ExchangeAwaitable awaitable(host, sdk::HttpRequest{});
        yaddnsc_http_response response{};
        response.status_code = 204;
        awaitable.set_response(response);
        result = awaitable.await_resume();
    }
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 204u);
    EXPECT_TRUE(result->body.empty());
    EXPECT_TRUE(result->headers.empty());
    {
        sdk::ExchangeAwaitable awaitable(host, sdk::HttpRequest{});
        std::string message = "temporary transport failure";
        auto error = make_error();
        error.message = {message.data(), message.size()};
        error.retry_after_seconds = 7;
        awaitable.set_error(YADDNSC_STATUS_NETWORK_ERROR, error);
        result = awaitable.await_resume();
    }
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(result.error().message, "temporary transport failure");
    EXPECT_EQ(result.error().retry_after_seconds, 7u);
}

TEST(SdkAsyncResponseOwnership, OwnedHeadersUseTheSameStatusClassification) {
    sdk::AsyncHttpResponse response{429, {{"rEtRy-AfTeR", " 45 "}, {"Retry-After", "90"}}, {}};
    auto failure = sdk::detail::classify_upstream_failure(response);
    EXPECT_EQ(failure.status, YADDNSC_STATUS_RATE_LIMITED);
    EXPECT_EQ(failure.retry_after_seconds, 45u);
    response.headers[0].value = "invalid";
    EXPECT_EQ(sdk::detail::retry_after_seconds(response), 0u);
    for (const auto status : {401u, 403u, 400u, 500u}) {
        response.status_code = status;
        failure = sdk::detail::classify_upstream_failure(response);
        EXPECT_EQ(failure.status, status == 401u || status == 403u ? YADDNSC_STATUS_AUTHENTICATION_FAILED
                                                                   : YADDNSC_STATUS_UPSTREAM_REJECTED);
        EXPECT_EQ(failure.retry_after_seconds, 0u);
    }
}

TEST(SdkAsyncValidate, IsOneShotAndBlocksBegin) {
    auto services = make_services();
    sdk::detail::AsyncInstance<ImmediateDriver> instance(&services);
    yaddnsc_error error = make_error();
    const yaddnsc_string json{"{}", 2};

    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    auto request = make_request();
    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
}

TEST(SdkAsyncValidate, IsRejectedAfterAnUpdateStarts) {
    auto services = make_services();
    sdk::detail::AsyncInstance<ImmediateDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    const yaddnsc_string json{"{}", 2};

    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
}

TEST(SdkAsyncValidate, IsRejectedWhileAnUpdateIsPending) {
    auto services = make_services();
    sdk::detail::AsyncInstance<PendingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    const yaddnsc_string json{"{}", 2};

    EXPECT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(instance.validate(json, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
}

TEST(SdkAsyncEventPayload, CancellationRequiresTheCompletePayloadShape) {
    auto services = make_services();
    sdk::detail::AsyncInstance<PendingDriver> instance(&services);
    yaddnsc_error error = make_error();
    auto request = make_request();
    ASSERT_EQ(instance.begin(request, &error), YADDNSC_STATUS_PENDING);

    yaddnsc_error cancel_error{};
    auto event = make_cancel_event(cancel_error);
    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    event.response = &response;
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    cancel_error = yaddnsc_error{};
    event = make_cancel_event(cancel_error);
    cancel_error.struct_size = YADDNSC_ERROR_MIN_SIZE - 1;
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    cancel_error = yaddnsc_error{};
    event = make_cancel_event(cancel_error);
    cancel_error.status = YADDNSC_STATUS_INTERNAL_ERROR;
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    cancel_error = yaddnsc_error{};
    event = make_cancel_event(cancel_error);
    EXPECT_EQ(instance.resume(event, &error), YADDNSC_STATUS_CANCELLED);
}
