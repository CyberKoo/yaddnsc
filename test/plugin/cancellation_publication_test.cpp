// Isolated allocation rendezvous reproduces cancellation after the Host Services
// entry check but before request publication, without production test hooks.
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "application/ports/log.h"
#include "coro/coro.h"
#include "coro/host.hpp"
#include "domain/update/driver_update_command.h"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/bridge.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_gateway.h"
#include "infrastructure/plugin/host_services.h"
#include "mocks/allocation_fault.h"
#include "mocks/null_logger.h"
#include "yaddnsc/sdk/driver_abi.h"

namespace {
thread_local bool stop_at_allocation = false;
std::atomic<bool> allocation_reached{false};
std::atomic<bool> release_allocation{false};
thread_local int allocations_before_failure = -1;
thread_local std::size_t failure_size = 0;
thread_local bool measure_next_allocation = false;
std::size_t bridge_call_allocation_size = 0;

void allocation_hook(std::size_t size) {
    if (measure_next_allocation) {
        bridge_call_allocation_size = size;
        measure_next_allocation = false;
    }
    if (allocations_before_failure == 0 && (failure_size == 0 || size == failure_size)) {
        allocations_before_failure = -1;
        failure_size = 0;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
    if (stop_at_allocation) {
        stop_at_allocation = false;
        allocation_reached.store(true);
        release_allocation.wait(false);
    }
}

// Arm this binary's allocation-fault hook before any test runs.
[[maybe_unused]] const bool allocation_hook_armed = [] {
    test::set_allocation_fault_hook(&allocation_hook);
    return true;
}();
}  // namespace

TEST(PluginCancellationPublication, CancelDuringRequestCopy_LoopRejectsLateAdmission) {
    allocation_reached.store(false);
    release_allocation.store(false);
    coro::Loop loop;
    NullLogger logger;
    yaddnsc_status status = YADDNSC_STATUS_OK;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            auto cycle = std::make_shared<plugin::CallState>();
            cycle->lifetime = coro::group_token(group);
            auto bridge = std::make_shared<plugin::Bridge>(loop, group, http::Options{}, std::chrono::seconds{5});
            co_await group.with_children([&]() -> coro::Task<void> {
                auto worker = coro::offload_joined([&] {
                    plugin::HostServicesContext context{*bridge, logger, cycle};
                    context.set_phase(plugin::HostServicesContext::Phase::UPDATE);
                    const auto services = context.make_services();
                    constexpr std::string_view url = "http://127.0.0.1:1/never-admit";
                    yaddnsc_http_request request{};
                    request.struct_size = sizeof(request);
                    request.url = {url.data(), url.size()};
                    request.method = YADDNSC_HTTP_GET;
                    yaddnsc_http_response response{};
                    response.struct_size = sizeof(response);
                    yaddnsc_error error{};
                    error.struct_size = sizeof(error);
                    stop_at_allocation = true;
                    return services.http_exchange(services.context, &request, &response, &error);
                });
                auto run_worker = [&]() -> coro::Task<void> { status = co_await std::move(worker); };
                group.spawn(run_worker());
                while (!allocation_reached.load()) {
                    co_await coro::checkpoint();
                }
                // HostServices passed is_cancelled(), but has not built a BridgeCall.
                cycle->request_cancel();
                release_allocation.store(true);
                release_allocation.notify_one();
                co_await group.join();
                EXPECT_TRUE(cycle->calls.empty());
            });
        });
    };
    coro::run(loop, root());
    EXPECT_EQ(status, YADDNSC_STATUS_CANCELLED);
}

TEST(PluginAllocationFailure, PostAllocationFailure_ReportsWorkerDefectWithoutRetainedRequest) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            auto cycle = std::make_shared<plugin::CallState>();
            cycle->lifetime = coro::group_token(group);
            auto bridge = std::make_shared<plugin::Bridge>(loop, group, http::Options{}, std::chrono::seconds{5});
            auto call = std::make_shared<plugin::BridgeCall>();
            call->cycle = cycle;
            call->url = "http://127.0.0.1/";
            const std::weak_ptr<plugin::BridgeCall> weak = call;
            auto response = co_await coro::offload_joined([bridge, call = std::move(call)] {
                // Fail the first worker-thread allocation inside exchange(): the
                // posted ingress closure (or its queue node). Its footprint is
                // standard-library dependent, so arm a wildcard, not a size.
                allocations_before_failure = 0;
                auto value = bridge->exchange(call);
                allocations_before_failure = -1;
                return value;
            });
            EXPECT_FALSE(response);
            EXPECT_EQ(response.error().code, YADDNSC_STATUS_INTERNAL_ERROR);
            EXPECT_TRUE(cycle->worker_error);
            EXPECT_TRUE(cycle->calls.empty());
            EXPECT_TRUE(weak.expired());
        });
    };
    coro::run(loop, root());
}

TEST(PluginAllocationFailure, LoopAdmissionAllocationFailure_CompletesPromiseAndReleasesRequest) {
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            auto cycle = std::make_shared<plugin::CallState>();
            cycle->lifetime = coro::group_token(group);
            auto bridge = std::make_shared<plugin::Bridge>(loop, group, http::Options{}, std::chrono::seconds{5});
            auto call = std::make_shared<plugin::BridgeCall>();
            call->cycle = cycle;
            call->url = "http://127.0.0.1/";
            const std::weak_ptr<plugin::BridgeCall> weak = call;
            const auto response = co_await coro::offload_joined([&loop, bridge, call = std::move(call)] {
                // Arm after the offload state (including stop_source) exists,
                // before the following ingress callback can grow its live set.
                loop.post([] {
                    failure_size = sizeof(std::weak_ptr<plugin::BridgeCall>);
                    allocations_before_failure = 0;
                });
                return bridge->exchange(call);
            });
            allocations_before_failure = -1;
            failure_size = 0;
            EXPECT_FALSE(response);
            EXPECT_EQ(response.error().code, YADDNSC_STATUS_INTERNAL_ERROR);
            EXPECT_TRUE(cycle->error);
            EXPECT_FALSE(cycle->worker_error);
            EXPECT_TRUE(cycle->calls.empty());
            EXPECT_TRUE(weak.expired());
        });
    };
    coro::run(loop, root());
}

namespace {
class AllocationFaultLogger final : public app::LoggerPort {
public:
    [[nodiscard]] bool is_enabled(app::LogLevel) const override { return true; }

    void log(app::LogLevel, std::string_view, const std::source_location&) const override {}

    void log_explicit(app::LogLevel, std::string_view message, std::string_view, int, std::string_view) const override {
        if (message == "inject host request allocation failure") {
            failure_size = bridge_call_allocation_size;
            allocations_before_failure = 0;
        }
    }
};
}  // namespace

TEST(PluginAllocationFailure, Gateway_HostRequestCopyFailure_RethrownAfterActualCycleDrain) {
    DriverCatalog catalog;
    ASSERT_NO_THROW(catalog.load_driver(TEST_PLUGIN_PATH));
    AllocationFaultLogger logger;
    // Measure make_shared's control block on this standard library, rather
    // than assuming a layout or failing the plugin's SDK request marshalling.
    measure_next_allocation = true;
    const auto probe = std::make_shared<plugin::BridgeCall>();
    ASSERT_GT(bridge_call_allocation_size, sizeof(plugin::BridgeCall));
    coro::Loop loop;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            plugin::DriverGateway gateway(catalog, logger, loop, group);
            co_await group.with_children([&]() -> coro::Task<void> {
                domain::DriverUpdateCommand command{
                    .driver_params = R"({"op":"log_then_exchange","url":"http://127.0.0.1/"})",
                    .ip_addr = "192.0.2.1",
                    .rd_type = "A",
                    .domain = "example.test",
                    .subdomain = "www",
                    .fqdn = "www.example.test"};
                EXPECT_THROW(co_await gateway.update("test_driver_plugin", std::move(command)), std::bad_alloc);
            });
        });
    };
    EXPECT_NO_THROW(coro::run(loop, root()));
}
