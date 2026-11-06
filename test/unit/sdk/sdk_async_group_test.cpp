#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>

namespace sdk = yaddnsc::sdk;

namespace {

struct FakeHost {
    yaddnsc_status start_status = YADDNSC_STATUS_OK;
    uint64_t next_op_id = 1;
    int start_calls = 0;
    int fail_start_call = 0;
    yaddnsc_status cancel_status = YADDNSC_STATUS_OK;
    int fail_cancel_call = 0;
    bool cancelled = false;
    std::vector<uint64_t> cancel_calls;
};

void fake_log(void*, yaddnsc_log_level, yaddnsc_string, const yaddnsc_source_location*) {}

int32_t fake_log_enabled(void*, yaddnsc_log_level) {
    return 0;
}

int32_t fake_cancelled(void* context) {
    const auto& host = *static_cast<const FakeHost*>(context);
    return host.cancelled ? 1 : 0;
}

yaddnsc_status fake_start(void* context, const yaddnsc_http_request*, uint64_t* out_op_id, yaddnsc_error* out_error) {
    auto& host = *static_cast<FakeHost*>(context);
    ++host.start_calls;
    if (host.start_status != YADDNSC_STATUS_OK &&
        (host.fail_start_call == 0 || host.start_calls == host.fail_start_call)) {
        static constexpr std::string_view message{"host refused the request"};
        out_error->status = host.start_status;
        out_error->message = {message.data(), message.size()};
        return host.start_status;
    }
    *out_op_id = host.next_op_id++;
    return YADDNSC_STATUS_OK;
}

yaddnsc_status fake_cancel(void* context, uint64_t op_id, yaddnsc_error* out_error) {
    auto& host = *static_cast<FakeHost*>(context);
    host.cancel_calls.push_back(op_id);
    if (host.cancel_status != YADDNSC_STATUS_OK &&
        (host.fail_cancel_call == 0 || static_cast<int>(host.cancel_calls.size()) == host.fail_cancel_call)) {
        static constexpr std::string_view message{"host refused cancellation"};
        out_error->status = host.cancel_status;
        out_error->message = {message.data(), message.size()};
        return host.cancel_status;
    }
    return YADDNSC_STATUS_OK;
}

yaddnsc_async_services make_services(FakeHost& host) {
    yaddnsc_async_services services{};
    services.struct_size = static_cast<uint32_t>(sizeof(services));
    services.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    services.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    services.context = &host;
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

yaddnsc_event make_event(uint64_t op_id, yaddnsc_status status) {
    yaddnsc_event event{};
    event.struct_size = static_cast<uint32_t>(sizeof(event));
    event.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    event.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    event.kind = YADDNSC_EVENT_HTTP_FINISHED;
    event.op_id = op_id;
    event.status = status;
    return event;
}

enum class GroupMode {
    ALL,
    ANY,
};

GroupMode g_mode = GroupMode::ALL;
sdk::AsyncWhenAnyPolicy g_policy = sdk::AsyncWhenAnyPolicy::FIRST_COMPLETED;
std::size_t g_request_count = 3;
std::vector<sdk::AsyncExchangeResult> g_results;
std::optional<std::size_t> g_winner;
bool g_update_cancelled = false;
bool g_catch_group_error = false;
bool g_group_error_caught = false;

class GroupDriver final : public sdk::AsyncDriver {
public:
    sdk::Task<sdk::Result> update(sdk::AsyncUpdateContext& context) override {
        std::vector<sdk::HttpRequest> requests;
        requests.reserve(g_request_count);
        for (std::size_t i = 0; i < g_request_count; ++i) {
            sdk::HttpRequest request;
            request.url = "https://example.test/" + std::to_string(i);
            requests.push_back(std::move(request));
        }

        if (g_mode == GroupMode::ALL) {
            auto operation = context.when_all(std::move(requests));
            if (!operation) {
                co_return failure(operation.error());
            }
            auto outcome = co_await std::move(*operation);
            g_update_cancelled = outcome.update_cancelled_observed;
            g_results = std::move(outcome.results);
        } else {
            auto operation = context.when_any(std::move(requests), g_policy);
            if (!operation) {
                co_return failure(operation.error());
            }
            if (g_catch_group_error) {
                try {
                    auto outcome = co_await std::move(*operation);
                    g_results = std::move(outcome.results);
                } catch (const sdk::detail::AsyncGroupCancellationError&) {
                    g_group_error_caught = true;
                }
                co_return {};
            }
            auto outcome = co_await std::move(*operation);
            g_winner = outcome.winner_index;
            g_update_cancelled = outcome.update_cancelled_observed;
            g_results = std::move(outcome.results);
        }
        co_return {};
    }

    [[nodiscard]] sdk::Result validate(std::string_view) const override { return {}; }

private:
    static sdk::Result failure(const sdk::HttpError& error) {
        return std::unexpected(sdk::Error{error.status, error.message, error.retry_after_seconds});
    }
};

class SdkAsyncGroupTest : public ::testing::Test {
protected:
    void SetUp() override {
        services_ = make_services(host_);
        g_mode = GroupMode::ALL;
        g_policy = sdk::AsyncWhenAnyPolicy::FIRST_COMPLETED;
        g_request_count = 3;
        g_results.clear();
        g_winner.reset();
        g_update_cancelled = false;
        g_catch_group_error = false;
        g_group_error_caught = false;
        instance_ = std::make_unique<sdk::detail::AsyncInstance<GroupDriver>>(&services_);
    }

    yaddnsc_status begin() { return instance_->begin(make_request(), &error_); }

    yaddnsc_status respond(uint64_t op_id, uint32_t status_code) {
        yaddnsc_http_response response{};
        response.struct_size = static_cast<uint32_t>(sizeof(response));
        response.status_code = status_code;
        auto event = make_event(op_id, YADDNSC_STATUS_OK);
        event.response = &response;
        return instance_->resume(event, &error_);
    }

    yaddnsc_status fail(uint64_t op_id, yaddnsc_status status) {
        auto payload = make_error();
        payload.status = status;
        static constexpr std::string_view message{"request failed"};
        payload.message = {message.data(), message.size()};
        auto event = make_event(op_id, status);
        event.error = &payload;
        return instance_->resume(event, &error_);
    }

    yaddnsc_status cancel_update() {
        auto payload = make_error();
        payload.status = YADDNSC_STATUS_CANCELLED;
        static constexpr std::string_view message{"update cancelled"};
        payload.message = {message.data(), message.size()};
        auto event = make_event(0, YADDNSC_STATUS_CANCELLED);
        event.kind = YADDNSC_EVENT_UPDATE_CANCELLED;
        event.error = &payload;
        return instance_->resume(event, &error_);
    }

    FakeHost host_;
    yaddnsc_async_services services_{};
    yaddnsc_error error_ = make_error();
    std::unique_ptr<sdk::detail::AsyncInstance<GroupDriver>> instance_;
};

void expect_error(const sdk::AsyncExchangeResult& result, yaddnsc_status status) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, status);
}

}  // namespace

TEST_F(SdkAsyncGroupTest, WhenAllOutOfOrderCompletionPreservesRequestOrder) {
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(host_.start_calls, 3);

    EXPECT_EQ(respond(2, 202), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(fail(1, YADDNSC_STATUS_NETWORK_ERROR), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(respond(3, 203), YADDNSC_STATUS_OK);

    ASSERT_EQ(g_results.size(), 3u);
    expect_error(g_results[0], YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(g_results[1]->status_code, 202u);
    EXPECT_EQ(g_results[2]->status_code, 203u);
    EXPECT_TRUE(host_.cancel_calls.empty());
}

TEST_F(SdkAsyncGroupTest, WhenAllEmptyAndSynchronousFailuresCompleteInline) {
    g_request_count = 0;
    EXPECT_EQ(begin(), YADDNSC_STATUS_OK);
    EXPECT_TRUE(g_results.empty());
    EXPECT_EQ(host_.start_calls, 0);
}

TEST_F(SdkAsyncGroupTest, WhenAllSynchronousFailuresDoNotSuspend) {
    host_.start_status = YADDNSC_STATUS_NETWORK_ERROR;
    EXPECT_EQ(begin(), YADDNSC_STATUS_OK);
    ASSERT_EQ(g_results.size(), 3u);
    for (const auto& result : g_results) {
        expect_error(result, YADDNSC_STATUS_NETWORK_ERROR);
    }
}

TEST_F(SdkAsyncGroupTest, GroupCreationRejectsExcessCapacityBeforeStartingAnything) {
    g_request_count = sdk::MAX_ASYNC_EXCHANGES_PER_INSTANCE + 1;
    EXPECT_EQ(begin(), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(host_.start_calls, 0);
    EXPECT_TRUE(host_.cancel_calls.empty());
    EXPECT_TRUE(g_results.empty());
}

TEST_F(SdkAsyncGroupTest, UnstartedGroupReleasesEveryReservation) {
    const auto requests = [](std::size_t count) {
        std::vector<sdk::HttpRequest> value(count);
        for (auto& request : value) {
            request.url = "https://example.test/";
        }
        return value;
    };
    sdk::AsyncServices api(&services_, instance_.get());

    {
        auto operation = api.when_all(requests(sdk::MAX_ASYNC_EXCHANGES_PER_INSTANCE));
        ASSERT_TRUE(operation.has_value());
        auto rejected = api.when_all(requests(1));
        ASSERT_FALSE(rejected.has_value());
        EXPECT_EQ(rejected.error().status, YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(host_.start_calls, 0);
    }

    auto accepted = api.when_all(requests(1));
    EXPECT_TRUE(accepted.has_value());
    EXPECT_EQ(host_.start_calls, 0);
}

TEST_F(SdkAsyncGroupTest, PartialGroupReservationRollsBackBeforeReturningError) {
    sdk::AsyncServices api(&services_, instance_.get());
    std::vector<sdk::HttpRequest> requests(sdk::MAX_ASYNC_EXCHANGES_PER_INSTANCE);

    {
        auto single = api.exchange(sdk::HttpRequest{});
        auto rejected = api.when_all(std::move(requests));
        ASSERT_FALSE(rejected.has_value());
        EXPECT_EQ(rejected.error().status, YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(host_.start_calls, 0);
    }

    auto accepted = api.when_all(std::vector<sdk::HttpRequest>(sdk::MAX_ASYNC_EXCHANGES_PER_INSTANCE));
    EXPECT_TRUE(accepted.has_value());
    EXPECT_EQ(host_.start_calls, 0);
}

TEST_F(SdkAsyncGroupTest, WhenAnyFirstCompletedErrorWinsAndDrainsLosers) {
    g_mode = GroupMode::ANY;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);

    EXPECT_EQ(fail(2, YADDNSC_STATUS_NETWORK_ERROR), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(host_.cancel_calls, (std::vector<uint64_t>{1, 3}));
    EXPECT_TRUE(g_results.empty());
    EXPECT_EQ(fail(1, YADDNSC_STATUS_CANCELLED), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(fail(3, YADDNSC_STATUS_CANCELLED), YADDNSC_STATUS_OK);

    ASSERT_EQ(g_winner, 1u);
    ASSERT_EQ(g_results.size(), 3u);
    expect_error(g_results[0], YADDNSC_STATUS_CANCELLED);
    expect_error(g_results[1], YADDNSC_STATUS_NETWORK_ERROR);
    expect_error(g_results[2], YADDNSC_STATUS_CANCELLED);
}

TEST_F(SdkAsyncGroupTest, CancelRejectionAttemptsEveryLoserAndReleasesEverySlot) {
    g_mode = GroupMode::ANY;
    g_request_count = 4;
    host_.cancel_status = YADDNSC_STATUS_INTERNAL_ERROR;
    host_.fail_cancel_call = 2;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);

    EXPECT_EQ(respond(2, 202), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(host_.cancel_calls, (std::vector<uint64_t>{1, 3, 4}));
    EXPECT_TRUE(g_results.empty());
    const std::string message(error_.message.data, error_.message.size);
    EXPECT_NE(message.find("op 3"), std::string::npos);
    EXPECT_NE(message.find("status 10"), std::string::npos);
    EXPECT_EQ(message.find("op 1"), std::string::npos);
    EXPECT_EQ(message.find("op 4"), std::string::npos);
    EXPECT_EQ(respond(1, 201), YADDNSC_STATUS_INVALID_ARGUMENT);

    std::array<sdk::detail::ExchangeSlotLease, sdk::MAX_ASYNC_EXCHANGES_PER_INSTANCE> leases;
    for (auto& lease : leases) {
        auto acquired = sdk::detail::acquire_slot(*instance_);
        ASSERT_TRUE(acquired.has_value());
        lease = std::move(*acquired);
    }
}

TEST_F(SdkAsyncGroupTest, BeginCancelRejectionAfterSynchronousWinnerTerminatesUpdate) {
    g_mode = GroupMode::ANY;
    host_.start_status = YADDNSC_STATUS_NETWORK_ERROR;
    host_.fail_start_call = 1;
    host_.cancel_status = YADDNSC_STATUS_INTERNAL_ERROR;

    EXPECT_EQ(begin(), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(host_.start_calls, 3);
    EXPECT_EQ(host_.cancel_calls, (std::vector<uint64_t>{1, 2}));
    EXPECT_TRUE(g_results.empty());
}

TEST_F(SdkAsyncGroupTest, DriverCannotSuppressLatchedCancelFailure) {
    g_mode = GroupMode::ANY;
    g_catch_group_error = true;
    host_.cancel_status = YADDNSC_STATUS_INTERNAL_ERROR;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);

    EXPECT_EQ(respond(2, 202), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_TRUE(g_group_error_caught);
    EXPECT_EQ(host_.cancel_calls, (std::vector<uint64_t>{1, 3}));
    EXPECT_TRUE(g_results.empty());
    const std::string message(error_.message.data, error_.message.size);
    EXPECT_NE(message.find("op 1"), std::string::npos);
    EXPECT_NE(message.find("op 3"), std::string::npos);
}

TEST_F(SdkAsyncGroupTest, WhenAnyFirstSuccessIgnoresEarlierErrors) {
    g_mode = GroupMode::ANY;
    g_policy = sdk::AsyncWhenAnyPolicy::FIRST_SUCCESS;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);

    EXPECT_EQ(fail(2, YADDNSC_STATUS_NETWORK_ERROR), YADDNSC_STATUS_PENDING);
    EXPECT_TRUE(host_.cancel_calls.empty());
    EXPECT_EQ(respond(3, 203), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(host_.cancel_calls, (std::vector<uint64_t>{1}));
    EXPECT_EQ(fail(1, YADDNSC_STATUS_CANCELLED), YADDNSC_STATUS_OK);

    ASSERT_EQ(g_winner, 2u);
    expect_error(g_results[0], YADDNSC_STATUS_CANCELLED);
    expect_error(g_results[1], YADDNSC_STATUS_NETWORK_ERROR);
    ASSERT_TRUE(g_results[2].has_value());
    EXPECT_EQ(g_results[2]->status_code, 203u);
}

TEST_F(SdkAsyncGroupTest, WhenAnyFirstSuccessReportsEveryFailure) {
    g_mode = GroupMode::ANY;
    g_policy = sdk::AsyncWhenAnyPolicy::FIRST_SUCCESS;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);

    EXPECT_EQ(fail(1, YADDNSC_STATUS_NETWORK_ERROR), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(fail(2, YADDNSC_STATUS_UPSTREAM_REJECTED), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(fail(3, YADDNSC_STATUS_RATE_LIMITED), YADDNSC_STATUS_OK);

    EXPECT_FALSE(g_winner.has_value());
    ASSERT_EQ(g_results.size(), 3u);
    expect_error(g_results[0], YADDNSC_STATUS_NETWORK_ERROR);
    expect_error(g_results[1], YADDNSC_STATUS_UPSTREAM_REJECTED);
    expect_error(g_results[2], YADDNSC_STATUS_RATE_LIMITED);
    EXPECT_TRUE(host_.cancel_calls.empty());
}

TEST_F(SdkAsyncGroupTest, WhenAnySynchronousWinnerDoesNotReenterLaunch) {
    g_mode = GroupMode::ANY;
    host_.start_status = YADDNSC_STATUS_NETWORK_ERROR;
    host_.fail_start_call = 1;

    EXPECT_EQ(begin(), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(host_.start_calls, 3);
    EXPECT_EQ(host_.cancel_calls, (std::vector<uint64_t>{1, 2}));
    EXPECT_TRUE(g_results.empty());

    EXPECT_EQ(fail(1, YADDNSC_STATUS_CANCELLED), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(fail(2, YADDNSC_STATUS_CANCELLED), YADDNSC_STATUS_OK);
    ASSERT_EQ(g_winner, 0u);
    expect_error(g_results[0], YADDNSC_STATUS_NETWORK_ERROR);
}

TEST_F(SdkAsyncGroupTest, UpdateCancellationCompletesEveryPendingChild) {
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(cancel_update(), YADDNSC_STATUS_OK);

    EXPECT_TRUE(g_update_cancelled);
    ASSERT_EQ(g_results.size(), 3u);
    for (const auto& result : g_results) {
        expect_error(result, YADDNSC_STATUS_CANCELLED);
    }
}

TEST_F(SdkAsyncGroupTest, WhenAnyUpdateCancellationClearsWinnerBeforeSelection) {
    g_mode = GroupMode::ANY;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);

    EXPECT_EQ(cancel_update(), YADDNSC_STATUS_OK);
    EXPECT_TRUE(g_update_cancelled);
    EXPECT_FALSE(g_winner.has_value());
    ASSERT_EQ(g_results.size(), 3u);
    for (const auto& result : g_results) {
        expect_error(result, YADDNSC_STATUS_CANCELLED);
    }
}

TEST_F(SdkAsyncGroupTest, WhenAnyUpdateCancellationClearsWinnerDuringDrain) {
    g_mode = GroupMode::ANY;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);
    ASSERT_EQ(respond(2, 202), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(host_.cancel_calls, (std::vector<uint64_t>{1, 3}));

    EXPECT_EQ(cancel_update(), YADDNSC_STATUS_OK);
    EXPECT_TRUE(g_update_cancelled);
    EXPECT_FALSE(g_winner.has_value());
    ASSERT_EQ(g_results.size(), 3u);
    expect_error(g_results[0], YADDNSC_STATUS_CANCELLED);
    expect_error(g_results[2], YADDNSC_STATUS_CANCELLED);
    ASSERT_TRUE(g_results[1].has_value());
    EXPECT_EQ(g_results[1]->status_code, 202u);
}

TEST_F(SdkAsyncGroupTest, DuplicateEventDoesNotConsumeAnotherSlot) {
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(respond(1, 201), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(respond(1, 201), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(respond(2, 202), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(respond(3, 203), YADDNSC_STATUS_OK);

    ASSERT_EQ(g_results.size(), 3u);
    EXPECT_EQ(g_results[0]->status_code, 201u);
    EXPECT_EQ(g_results[1]->status_code, 202u);
    EXPECT_EQ(g_results[2]->status_code, 203u);
}

TEST_F(SdkAsyncGroupTest, WhenAnyDrainsReadyLoserResponsesWithoutChangingWinner) {
    g_mode = GroupMode::ANY;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);

    EXPECT_EQ(respond(2, 202), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(host_.cancel_calls, (std::vector<uint64_t>{1, 3}));
    EXPECT_EQ(respond(1, 201), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(respond(3, 203), YADDNSC_STATUS_OK);

    ASSERT_EQ(g_winner, 1u);
    EXPECT_EQ(g_results[0]->status_code, 201u);
    EXPECT_EQ(g_results[1]->status_code, 202u);
    EXPECT_EQ(g_results[2]->status_code, 203u);
}

TEST_F(SdkAsyncGroupTest, WhenAnyAllSynchronousFailuresCompleteInline) {
    g_mode = GroupMode::ANY;
    g_policy = sdk::AsyncWhenAnyPolicy::FIRST_SUCCESS;
    host_.start_status = YADDNSC_STATUS_NETWORK_ERROR;

    EXPECT_EQ(begin(), YADDNSC_STATUS_OK);
    EXPECT_FALSE(g_winner.has_value());
    ASSERT_EQ(g_results.size(), 3u);
    for (const auto& result : g_results) {
        expect_error(result, YADDNSC_STATUS_NETWORK_ERROR);
    }
}

TEST_F(SdkAsyncGroupTest, WhenAnyEmptyReturnsWithoutWinner) {
    g_mode = GroupMode::ANY;
    g_policy = sdk::AsyncWhenAnyPolicy::FIRST_SUCCESS;
    g_request_count = 0;
    EXPECT_EQ(begin(), YADDNSC_STATUS_OK);
    EXPECT_FALSE(g_winner.has_value());
    EXPECT_TRUE(g_results.empty());
}

TEST_F(SdkAsyncGroupTest, WhenAllLiveCancellationBeforeStartReturnsCancellationWithoutStarting) {
    host_.cancelled = true;

    EXPECT_EQ(begin(), YADDNSC_STATUS_OK);
    EXPECT_TRUE(g_update_cancelled);
    EXPECT_EQ(host_.start_calls, 0);
    ASSERT_EQ(g_results.size(), 3u);
    for (const auto& result : g_results) {
        expect_error(result, YADDNSC_STATUS_CANCELLED);
    }
}

TEST_F(SdkAsyncGroupTest, WhenAnyLiveCancellationBeforeStartClearsWinnerWithoutStarting) {
    g_mode = GroupMode::ANY;
    host_.cancelled = true;

    EXPECT_EQ(begin(), YADDNSC_STATUS_OK);
    EXPECT_TRUE(g_update_cancelled);
    EXPECT_FALSE(g_winner.has_value());
    EXPECT_EQ(host_.start_calls, 0);
    ASSERT_EQ(g_results.size(), 3u);
    for (const auto& result : g_results) {
        expect_error(result, YADDNSC_STATUS_CANCELLED);
    }
}

TEST_F(SdkAsyncGroupTest, WhenAnyLiveCancellationDuringDrainClearsWinnerWithoutEvent) {
    g_mode = GroupMode::ANY;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);
    ASSERT_EQ(respond(2, 202), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(host_.cancel_calls, (std::vector<uint64_t>{1, 3}));

    host_.cancelled = true;
    EXPECT_EQ(fail(1, YADDNSC_STATUS_CANCELLED), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(fail(3, YADDNSC_STATUS_CANCELLED), YADDNSC_STATUS_OK);
    EXPECT_TRUE(g_update_cancelled);
    EXPECT_FALSE(g_winner.has_value());
    ASSERT_EQ(g_results.size(), 3u);
    expect_error(g_results[0], YADDNSC_STATUS_CANCELLED);
    expect_error(g_results[2], YADDNSC_STATUS_CANCELLED);
    ASSERT_TRUE(g_results[1].has_value());
    EXPECT_EQ(g_results[1]->status_code, 202u);
}

TEST_F(SdkAsyncGroupTest, PerOpCancellationCanWinWithoutUpdateCancellation) {
    g_mode = GroupMode::ANY;
    host_.start_status = YADDNSC_STATUS_CANCELLED;
    host_.fail_start_call = 1;
    ASSERT_EQ(begin(), YADDNSC_STATUS_PENDING);

    EXPECT_EQ(fail(1, YADDNSC_STATUS_CANCELLED), YADDNSC_STATUS_PENDING);
    EXPECT_EQ(fail(2, YADDNSC_STATUS_CANCELLED), YADDNSC_STATUS_OK);
    EXPECT_FALSE(g_update_cancelled);
    ASSERT_EQ(g_winner, 0u);
    expect_error(g_results[0], YADDNSC_STATUS_CANCELLED);
}

TEST_F(SdkAsyncGroupTest, EmptyGroupObservesLiveCancellation) {
    g_mode = GroupMode::ANY;
    g_request_count = 0;
    host_.cancelled = true;

    EXPECT_EQ(begin(), YADDNSC_STATUS_OK);
    EXPECT_TRUE(g_update_cancelled);
    EXPECT_FALSE(g_winner.has_value());
    EXPECT_TRUE(g_results.empty());
    EXPECT_EQ(host_.start_calls, 0);
}
