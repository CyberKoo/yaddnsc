#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/notification.hpp"
#include "coro/scope.hpp"
#include "coro/task.hpp"
#include "infrastructure/plugin/async/cleanup.h"
#include "infrastructure/plugin/async/services.h"
#include "infrastructure/plugin/async/step.h"
#include "infrastructure/plugin/detail/cycle.h"
#include "infrastructure/plugin/plugin_loader.h"

#include "cycle.h"

namespace app {
class LoggerPort;
}  // namespace app

namespace coro {
class CancelScope;
}  // namespace coro

namespace plugin {
namespace {

/// Validation has no HTTP nursery or update-command state. Its stack context
/// survives every joined step, including destroy, before result publication.
coro::Task<void> async_validate_body(std::shared_ptr<CallState> state, std::shared_ptr<const PluginModule> module,
                                     std::string driver_param_json, const app::LoggerPort& logger) {
    HostAsyncServicesContext services{logger, state};
    yaddnsc_async_driver* handle = nullptr;
    const std::string_view driver_name = module->descriptor().name;
    auto destroy = [&]() -> coro::Task<void> {
        if (handle != nullptr) {
            co_await detail::run_async_step([&] {
                services.set_phase(HostAsyncServicesContext::Phase::DESTROY);
                module->async_destroy(handle);
                handle = nullptr;
                return detail::make_step_outcome(YADDNSC_STATUS_OK, yaddnsc_error{});
            });
        }
        co_return;
    };
    try {
        if (state->cancelled.load(std::memory_order_acquire)) {
            state->plugin_cancelled = true;
        } else {
            auto create = co_await detail::run_async_step([&] {
                yaddnsc_error error{};
                error.struct_size = static_cast<uint32_t>(sizeof(error));
                services.set_phase(HostAsyncServicesContext::Phase::CREATE);
                const auto status = module->async_create(services.services(), &handle, error);
                return detail::make_step_outcome(status, error);
            });
            if (detail::finish_async_step(*state, driver_name, create, "async create")) {
                if (state->cancelled.load(std::memory_order_acquire)) {
                    state->plugin_cancelled = true;
                } else {
                    const yaddnsc_string param{driver_param_json.data(), driver_param_json.size()};
                    auto validate = co_await detail::run_async_step([&] {
                        yaddnsc_error error{};
                        error.struct_size = static_cast<uint32_t>(sizeof(error));
                        services.set_phase(HostAsyncServicesContext::Phase::VALIDATE);
                        const auto status = module->async_validate(handle, param, error);
                        return detail::make_step_outcome(status, error);
                    });
                    detail::finish_async_step(*state, driver_name, validate, "async validate");
                }
            }
        }
        co_await destroy();
    } catch (...) {
        if (!state->error) {
            state->error = std::current_exception();
        }
    }
    if (handle != nullptr) {
        try {
            co_await destroy();
        } catch (...) {
            fatal_async_cleanup_failure(driver_name, "async validate destroy step did not complete");
        }
    }
    // Create failures also reach publication; never strand the subscriber.
    state->result_ready.notify();
    while (state->subscriber_active) {
        co_await state->owner_changed.wait();
    }
}

}  // namespace

coro::Task<void> make_async_validate_owner(std::shared_ptr<CallState> state, std::shared_ptr<const PluginModule> module,
                                           std::string driver_param_json, const app::LoggerPort& logger) {
    auto shielded = coro::non_cancellable(
        [body = async_validate_body(state, std::move(module), std::move(driver_param_json), logger)](
            coro::CancelScope&) mutable { return std::move(body); });
    return own_cycle(std::move(state), std::move(shielded));
}

}  // namespace plugin
