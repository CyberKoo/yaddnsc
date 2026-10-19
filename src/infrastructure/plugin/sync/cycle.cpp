#include "cycle.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <string_view>
#include <utility>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/host.hpp"
#include "coro/nursery.hpp"
#include "coro/offload.hpp"
#include "coro/scope.hpp"
#include "coro/with_nursery.hpp"
#include "domain/error/error.h"
#include "infrastructure/plugin/detail/abi_result.h"
#include "infrastructure/plugin/detail/abi_string.h"
#include "infrastructure/plugin/detail/cycle.h"
#include "infrastructure/plugin/plugin_loader.h"
#include "infrastructure/plugin/sync/bridge.h"
#include "infrastructure/plugin/sync/driver_instance.h"
#include "infrastructure/plugin/sync/host_services.h"

namespace plugin {
namespace {

/// Declared after DriverInstance: unwinding switches phase before destroying it.
struct DestroyPhase {
    HostServicesContext& context;

    ~DestroyPhase() noexcept { context.set_phase(HostServicesContext::Phase::DESTROY); }
};

std::expected<void, domain::DriverError> run_update_task(const std::shared_ptr<const PluginModule>& module,
                                                         const domain::DriverUpdateCommand& command, Bridge& bridge,
                                                         const app::LoggerPort& logger,
                                                         const std::shared_ptr<CallState>& state) {
    const std::string_view driver_name = module->descriptor().name;
    // Covers cancellation between offload's admission gate and the ABI cycle.
    if (state->cancelled.load(std::memory_order_acquire)) {
        throw detail::AbiCancelled{};
    }
    HostServicesContext context{bridge, logger, state};
    const auto services = context.make_services_v1_1();
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_driver* handle = nullptr;
    const auto create_status = module->create(services.base, &handle, error);
    if (create_status == YADDNSC_STATUS_OK && handle == nullptr) {
        return std::unexpected(detail::invalid_plugin_result(driver_name, "create"));
    }
    if (create_status != YADDNSC_STATUS_OK) {
        if (!detail::has_valid_plugin_error(error, create_status)) {
            return std::unexpected(detail::invalid_plugin_result(driver_name, "create"));
        }
        return std::unexpected(
            detail::map_error(create_status, detail::to_view(error.message), driver_name, command.fqdn));
    }
    // Error bytes are copied before destroy; contexts outlive the instance.
    DriverInstance instance{module, handle};
    DestroyPhase destroy_phase{context};
    const yaddnsc_update_request request{
        .struct_size = static_cast<uint32_t>(sizeof(request)),
        .ip_address = {command.ip_addr.data(), command.ip_addr.size()},
        .record_type = {command.rd_type.data(), command.rd_type.size()},
        .domain = {command.domain.data(), command.domain.size()},
        .subdomain = {command.subdomain.data(), command.subdomain.size()},
        .fqdn = {command.fqdn.data(), command.fqdn.size()},
        .driver_param_json = {reinterpret_cast<const uint8_t*>(command.driver_params.data()),
                              command.driver_params.size()},
    };
    error = {};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    context.set_phase(HostServicesContext::Phase::UPDATE);
    const auto status = instance.update(request, error);
    context.set_phase(HostServicesContext::Phase::DESTROY);
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }
    if (!detail::has_valid_plugin_error(error, status)) {
        return std::unexpected(detail::invalid_plugin_result(driver_name, "update"));
    }
    auto driver_error = detail::map_error(status, detail::to_view(error.message), driver_name, command.fqdn);
    driver_error.retry_after_seconds = static_cast<int>(std::min<std::uint32_t>(
        error.retry_after_seconds, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
    return std::unexpected(std::move(driver_error));
}

std::expected<void, domain::DriverError> run_validate_cycle(const std::shared_ptr<const PluginModule>& module,
                                                            const std::string& driver_param_json, Bridge& bridge,
                                                            const app::LoggerPort& logger,
                                                            const std::shared_ptr<CallState>& state) {
    const std::string_view driver_name = module->descriptor().name;
    if (state->cancelled.load(std::memory_order_acquire)) {
        throw detail::AbiCancelled{};
    }
    HostServicesContext context{bridge, logger, state};
    const auto services = context.make_services_v1_1();
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_driver* handle = nullptr;
    const auto create_status = module->create(services.base, &handle, error);
    if (create_status == YADDNSC_STATUS_OK && handle == nullptr) {
        return std::unexpected(detail::invalid_plugin_result(driver_name, "create"));
    }
    if (create_status != YADDNSC_STATUS_OK) {
        if (!detail::has_valid_plugin_error(error, create_status)) {
            return std::unexpected(detail::invalid_plugin_result(driver_name, "create"));
        }
        return std::unexpected(detail::map_validate_error(create_status, detail::to_view(error.message), driver_name));
    }
    DriverInstance instance{module, handle};
    DestroyPhase destroy_phase{context};
    error = {};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const yaddnsc_string param{driver_param_json.data(), driver_param_json.size()};
    context.set_phase(HostServicesContext::Phase::VALIDATE);
    const auto status = instance.validate(param, error);
    context.set_phase(HostServicesContext::Phase::DESTROY);
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }
    if (!detail::has_valid_plugin_error(error, status)) {
        return std::unexpected(detail::invalid_plugin_result(driver_name, "validate"));
    }
    return std::unexpected(detail::map_validate_error(status, detail::to_view(error.message), driver_name));
}

template<typename Work>
coro::Task<void> cycle_worker(std::shared_ptr<CallState> state, Work work) {
    try {
        auto outcome = co_await coro::with_cancel_scope(
            [&](coro::CancelScope& scope) -> coro::Task<std::expected<void, domain::DriverError>> {
                state->worker_scope = &scope;
                struct ScopeBorrow {
                    CallState& state;
                    ~ScopeBorrow() noexcept { state.worker_scope = nullptr; }
                };
                const ScopeBorrow borrow{*state};
                if (state->cancelled.load(std::memory_order_acquire)) {
                    scope.cancel();
                }
                co_return co_await coro::offload_joined(std::move(work));
            });
        if (!outcome.cancelled) {
            state->result = std::move(*outcome);
        } else {
            state->plugin_cancelled = true;
        }
    } catch (const detail::AbiCancelled&) {
        state->plugin_cancelled = true;
    } catch (const coro::Cancelled&) {
        state->request_cancel();
        state->plugin_cancelled = true;
    } catch (...) {
        if (!state->error) {
            state->error = std::current_exception();
        }
    }
    if (state->worker_error && !state->error) {
        state->error = state->worker_error;
    }
    state->worker_done = true;
    state->owner_changed.notify();
}

/// Shielded through worker/HTTP drain and subscriber acknowledgement.
template<typename Work>
coro::Task<void> cycle_body(std::shared_ptr<CallState> state, Work work) {
    try {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                state->lifetime = coro::nursery_token(group);
                group.spawn_discard(cycle_worker(state, std::move(work)));
                while (!state->worker_done) {
                    if (state->cancelled.load(std::memory_order_acquire)) {
                        if (state->worker_scope != nullptr) {
                            state->worker_scope->cancel();
                        }
                        for (const auto& tracked : state->calls) {
                            if (const auto call = tracked.lock()) {
                                call->cancelled.store(true, std::memory_order_release);
                                if (call->live_scope != nullptr) {
                                    call->live_scope->cancel();
                                }
                            }
                        }
                    }
                    co_await state->owner_changed.wait();
                }
                // Worker expiry can precede HTTP retirement. Close ingress on
                // body return, then let the nursery join the retiring children.
                for (const auto& tracked : state->calls) {
                    if (const auto call = tracked.lock()) {
                        call->cancelled.store(true, std::memory_order_release);
                        if (call->live_scope != nullptr) {
                            call->live_scope->cancel();
                        }
                    }
                }
            },
            coro::NurseryPolicy::SUPERVISOR);
    } catch (const coro::Cancelled&) {
        state->request_cancel();
        state->plugin_cancelled = true;
    } catch (...) {
        if (!state->error) {
            state->error = std::current_exception();
        }
    }
    state->result_ready.notify();
    while (state->subscriber_active) {
        co_await state->owner_changed.wait();
    }
}

/// Ordinary factory: all frames exist before start_cycle submits the owner.
template<typename Work>
coro::Task<void> make_cycle_owner(std::shared_ptr<CallState> state, Work work) {
    auto shielded = coro::non_cancellable(
        [body = cycle_body(state, std::move(work))](coro::CancelScope&) mutable { return std::move(body); });
    return own_cycle(std::move(state), std::move(shielded));
}

}  // namespace

coro::Task<void> make_sync_update_owner(std::shared_ptr<CallState> state, std::shared_ptr<const PluginModule> module,
                                        domain::DriverUpdateCommand command, std::shared_ptr<Bridge> bridge,
                                        const app::LoggerPort& logger) {
    return make_cycle_owner(
        state, [module = std::move(module), command = std::move(command), bridge = std::move(bridge), logger = &logger,
                state]() mutable { return run_update_task(module, command, *bridge, *logger, state); });
}

coro::Task<void> make_sync_validate_owner(std::shared_ptr<CallState> state, std::shared_ptr<const PluginModule> module,
                                          std::string driver_param_json, std::shared_ptr<Bridge> bridge,
                                          const app::LoggerPort& logger) {
    return make_cycle_owner(state, [module = std::move(module), param = std::move(driver_param_json),
                                    bridge = std::move(bridge), logger = &logger, state]() mutable {
        return run_validate_cycle(module, param, *bridge, *logger, state);
    });
}

}  // namespace plugin
