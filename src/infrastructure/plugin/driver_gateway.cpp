//
// plugin — the coroutine driver gateway over the v1 C ABI plugin host
// (implementation).
//

#include "driver_gateway.h"

#include <algorithm>
#include <atomic>
#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/fwd.h"
#include "coro/host.hpp"
#include "coro/notification.hpp"
#include "coro/nursery.hpp"
#include "coro/offload.hpp"
#include "coro/scope.hpp"
#include "coro/task.hpp"
#include "coro/with_nursery.hpp"
#include "domain/error/error.h"
#include "domain/update/driver_update_command.h"
#include "infrastructure/plugin/abi_string.h"
#include "infrastructure/plugin/bridge.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_instance.h"
#include "infrastructure/plugin/host_services.h"
#include "infrastructure/plugin/plugin_loader.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace plugin {

namespace {

// Synchronous worker-to-loop boundary marker; never crosses the C ABI.
struct AbiCancelled {};

[[nodiscard]] domain::DriverError map_error(yaddnsc_status status, std::string_view plugin_message,
                                            std::string_view driver_name, std::string_view fqdn) {
    using Code = domain::DriverError::Code;
    const std::string message = !plugin_message.empty()
                                    ? std::string(plugin_message)
                                    : fmt::format("Driver '{}' update failed for {}", driver_name, fqdn);
    switch (status) {
        case YADDNSC_STATUS_RATE_LIMITED:
            return {Code::RATE_LIMITED, message, 0};
        case YADDNSC_STATUS_CANCELLED:
            throw AbiCancelled{};
        case YADDNSC_STATUS_INVALID_CONFIG:
        case YADDNSC_STATUS_INTERNAL_ERROR:
            return {Code::UNKNOWN, message, 0};
        case YADDNSC_STATUS_NETWORK_ERROR:
        case YADDNSC_STATUS_AUTHENTICATION_FAILED:
        case YADDNSC_STATUS_UPSTREAM_REJECTED:
        case YADDNSC_STATUS_UNSUPPORTED_RECORD:
        case YADDNSC_STATUS_INVALID_RESPONSE:
            return {Code::UPDATE_FAILED, message, 0};
        default:
            // INVALID_ARGUMENT and unknown codes indicate a contract bug.
            return {Code::UNKNOWN, message, 0};
    }
}

[[nodiscard]] bool has_valid_plugin_error(const yaddnsc_error& error, yaddnsc_status returned_status) noexcept {
    return yaddnsc_status_is_valid(returned_status) && error.struct_size >= YADDNSC_ERROR_MIN_SIZE &&
           yaddnsc_status_is_valid(error.status) && error.status == returned_status &&
           yaddnsc_string_is_valid(error.message);
}

/// Bit required for an ABI 1.0 record type. "A" and "AAAA" have bits;
/// every other type, including "TXT", returns nullopt.
[[nodiscard]] std::optional<uint64_t> required_capability(std::string_view record_type) noexcept {
    if (record_type == "A") {
        return YADDNSC_DRIVER_CAPABILITY_A;
    }
    if (record_type == "AAAA") {
        return YADDNSC_DRIVER_CAPABILITY_AAAA;
    }
    return std::nullopt;
}

/// Host-side record-type gate. Runs before create(), so the plugin is not
/// called and no yaddnsc_status is produced. The domain result is
/// UPDATE_FAILED, the same code map_error uses when a plugin returns
/// YADDNSC_STATUS_UNSUPPORTED_RECORD.
[[nodiscard]] domain::DriverError unsupported_record(std::string_view driver_name, std::string_view record_type,
                                                     std::optional<uint64_t> required) {
    const std::string message =
        required.has_value()
            ? fmt::format("Driver '{}' does not support record type {} (missing capability {})", driver_name,
                          record_type, *required == YADDNSC_DRIVER_CAPABILITY_AAAA ? "AAAA" : "A")
            : fmt::format("Driver '{}' does not support record type {} (no v1 capability)", driver_name, record_type);
    return {domain::DriverError::Code::UPDATE_FAILED, message, 0};
}

[[nodiscard]] domain::DriverError invalid_plugin_result(std::string_view driver_name, std::string_view entry) {
    return {domain::DriverError::Code::UNKNOWN,
            fmt::format("Driver '{}' returned an invalid {} ABI error report", driver_name, entry), 0};
}

/// yaddnsc_status → domain::DriverError for the validate path. Unlike the
/// update mapping there is no fqdn context; any non-OK status means the
/// configuration was rejected (INVALID_CONFIG is the canonical code, but
/// plugins may report other failures — e.g. an internal error while
/// validating — which the caller must surface verbatim).
[[nodiscard]] domain::DriverError map_validate_error(yaddnsc_status status, std::string_view plugin_message,
                                                     std::string_view driver_name) {
    if (status == YADDNSC_STATUS_CANCELLED) {
        throw AbiCancelled{};
    }
    const std::string message = !plugin_message.empty()
                                    ? std::string(plugin_message)
                                    : fmt::format("Driver '{}' rejected its driver_params configuration", driver_name);
    return {domain::DriverError::Code::UNKNOWN, message, 0};
}

/// Declared after DriverInstance: unwinding switches phase before destroying it.
struct DestroyPhase {
    HostServicesContext& context;

    ~DestroyPhase() noexcept { context.set_phase(HostServicesContext::Phase::DESTROY); }
};

/// One create → update → destroy cycle, executed on an offload worker.
///
/// The module lease and the per-call state are shared_ptr parameters, so an
/// abandoned cycle keeps both alive until it finishes.
[[nodiscard]] std::expected<void, domain::DriverError> run_update_task(
    const std::shared_ptr<const PluginModule>& module, const domain::DriverUpdateCommand& command, Bridge& bridge,
    const app::LoggerPort& logger, const std::shared_ptr<CallState>& state) {
    const std::string_view driver_name = module->descriptor().name;

    // Offload drops an abandoned job before it starts; this gate covers the
    // race where the abandon lands between that check and the cycle's start:
    // the result is already discarded and the address may no longer be current.
    // Once the cycle has started it runs to completion.
    if (state->cancelled.load(std::memory_order_acquire)) {
        throw AbiCancelled{};
    }

    HostServicesContext context{bridge, logger, state};
    const auto services = context.make_services();

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    const yaddnsc_status create_status = module->create(services, &handle, error);
    if (create_status == YADDNSC_STATUS_OK && handle == nullptr) {
        return std::unexpected(invalid_plugin_result(driver_name, "create"));
    }
    if (create_status != YADDNSC_STATUS_OK) {
        if (!has_valid_plugin_error(error, create_status)) {
            return std::unexpected(invalid_plugin_result(driver_name, "create"));
        }
        return std::unexpected(map_error(create_status, detail::to_view(error.message), driver_name, command.fqdn));
    }

    // From here on the instance owns the destroy() call; copy error bytes out
    // synchronously after update() returns, before destroy.
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
    const yaddnsc_status status = instance.update(request, error);
    context.set_phase(HostServicesContext::Phase::DESTROY);
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }
    if (!has_valid_plugin_error(error, status)) {
        return std::unexpected(invalid_plugin_result(driver_name, "update"));
    }

    auto driver_error = map_error(status, detail::to_view(error.message), driver_name, command.fqdn);
    // The ABI field is uint32; clamp instead of narrowing so an out-of-range
    // plugin value saturates at INT_MAX rather than going negative.
    driver_error.retry_after_seconds = static_cast<int>(std::min<std::uint32_t>(
        error.retry_after_seconds, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
    return std::unexpected(std::move(driver_error));
}

/// One create → validate → destroy cycle, executed on an offload worker.
/// The services table refuses http_exchange (config test never touches the
/// network).
[[nodiscard]] std::expected<void, domain::DriverError> run_validate_cycle(
    const std::shared_ptr<const PluginModule>& module, const std::string& driver_param_json, Bridge& bridge,
    const app::LoggerPort& logger, const std::shared_ptr<CallState>& state) {
    const std::string_view driver_name = module->descriptor().name;

    // Same entry gate as the update cycle: an abandoned queued job skips the
    // plugin entirely.
    if (state->cancelled.load(std::memory_order_acquire)) {
        throw AbiCancelled{};
    }

    HostServicesContext context{bridge, logger, state};
    const auto services = context.make_services();

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    const yaddnsc_status create_status = module->create(services, &handle, error);
    if (create_status == YADDNSC_STATUS_OK && handle == nullptr) {
        return std::unexpected(invalid_plugin_result(driver_name, "create"));
    }
    if (create_status != YADDNSC_STATUS_OK) {
        if (!has_valid_plugin_error(error, create_status)) {
            return std::unexpected(invalid_plugin_result(driver_name, "create"));
        }
        return std::unexpected(map_validate_error(create_status, detail::to_view(error.message), driver_name));
    }

    DriverInstance instance{module, handle};

    DestroyPhase destroy_phase{context};

    error = {};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const yaddnsc_string param{driver_param_json.data(), driver_param_json.size()};
    context.set_phase(HostServicesContext::Phase::VALIDATE);
    const yaddnsc_status status = instance.validate(param, error);
    context.set_phase(HostServicesContext::Phase::DESTROY);
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }
    if (!has_valid_plugin_error(error, status)) {
        return std::unexpected(invalid_plugin_result(driver_name, "validate"));
    }
    return std::unexpected(map_validate_error(status, detail::to_view(error.message), driver_name));
}

/// A scope cancel hook signals the cycle; no runtime reentry in hooks.
void request_cancel_hook(void* owner) noexcept {
    static_cast<CallState*>(owner)->request_cancel();
}

/// Caller-side subscription to a cycle result. Armed before the owner starts,
/// so the owner's acknowledgement wait cannot miss it; released exactly once —
/// on normal return, cancellation or defect alike — which notifies the owner.
struct CycleSubscription {
    CallState& state;

    explicit CycleSubscription(CallState& cycle) noexcept : state(cycle) { state.subscriber_active = true; }

    CycleSubscription(const CycleSubscription&) = delete;
    CycleSubscription& operator=(const CycleSubscription&) = delete;

    ~CycleSubscription() noexcept {
        state.subscriber_active = false;
        state.owner_changed.notify();
    }
};

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
    } catch (const AbiCancelled&) {
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

/// The shielded cycle body: drain the real work, publish the outcome, and wait
/// for the subscriber's release. Everything past the nursery drain must not be
/// interrupted by root cancellation — a broken acknowledgement wait could drop
/// a late defect — so make_cycle_owner wraps this whole body in one shield.
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
                // A worker's budget may have expired while HTTP is still retiring.
                for (const auto& tracked : state->calls) {
                    if (const auto call = tracked.lock()) {
                        call->cancelled.store(true, std::memory_order_release);
                        if (call->live_scope != nullptr) {
                            call->live_scope->cancel();
                        }
                    }
                }
                // Body return closes ingress before the runner drains retiring HTTP.
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
    // Keep the error boundary alive through the subscriber's last cancellation
    // checkpoint: publishing a result does not prove it was observed.
    while (state->subscriber_active) {
        co_await state->owner_changed.wait();
    }
}

/// Parent scope and controller outlive a cancelled caller and all real work.
///
/// The shielded body is pre-built by make_cycle_owner: every frame allocation
/// happened before submission, so after it the shielded publish/acknowledge
/// epilogue is the only publication path — there is no fallback wait outside
/// the shield that a root cancellation could break (which would drop the
/// recorded defect).
coro::Task<void> own_cycle(std::shared_ptr<CallState> state, coro::Task<coro::ScopeOutcome<void>> shielded) {
    auto& parent = co_await coro::current_scope();
    const coro::CancelObserver cancellation{parent, state.get(), &request_cancel_hook};
    try {
        co_await std::move(shielded);
    } catch (const coro::Cancelled&) {
        state->request_cancel();
        state->plugin_cancelled = true;
    } catch (...) {
        if (!state->error) {
            state->error = std::current_exception();
        }
    }
    if (state->error && !state->error_observed) {
        std::rethrow_exception(state->error);
    }
}

/// Assemble an owned cycle: shielded body plus its owner wrapper. All frame
/// allocations happen here, before start_cycle submits the owner, so an
/// allocation failure propagates to the caller instead of leaving a running
/// owner without its shield.
template<typename Work>
coro::Task<void> make_cycle_owner(std::shared_ptr<CallState> state, Work work) {
    // The shield's callable only moves the pre-created body task.
    // `state` stays by value on purpose: this is a coroutine, so the frame —
    // not the caller — owns it through suspension.
    auto shielded = coro::non_cancellable(
        [body = cycle_body(state, std::move(work))](coro::CancelScope&) mutable { return std::move(body); });
    return own_cycle(std::move(state), std::move(shielded));
}

}  // namespace

DriverGateway::DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                             coro::Nursery& bridge_group)
    : DriverGateway(catalog, logger, loop, bridge_group, Options{}) {}

DriverGateway::DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                             coro::Nursery& bridge_group, Options options)
    : catalog_(catalog), logger_(logger),
      bridge_(std::make_shared<Bridge>(loop, bridge_group, std::move(options.http), options.bridge_wait_budget)) {}

coro::Task<std::expected<void, domain::DriverError>> DriverGateway::update(std::string driver_name,
                                                                           domain::DriverUpdateCommand command) {
    auto module = catalog_.find(driver_name);
    if (module == nullptr) {
        co_return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                      fmt::format("Driver '{}' is not loaded", driver_name)});
    }

    const auto required = required_capability(command.rd_type);
    if (!required.has_value() || (module->descriptor().capabilities & *required) == 0) {
        co_return std::unexpected(unsupported_record(driver_name, command.rd_type, required));
    }

    auto state = std::make_shared<CallState>();
    auto bridge = bridge_;
    const app::LoggerPort* const logger = &logger_;
    const std::string fqdn = command.fqdn;
    coro::CancelScope& scope = co_await coro::current_scope();
    scope.throw_if_cancelled();
    const coro::CancelObserver cancellation{scope, state.get(), &request_cancel_hook};
    const CycleSubscription subscription{*state};
    bridge->start_cycle(state, make_cycle_owner(state,
                                                [module, command = std::move(command), bridge, logger,
                                                 state]() mutable -> std::expected<void, domain::DriverError> {
                                                    return run_update_task(module, command, *bridge, *logger, state);
                                                }));
    try {
        co_await state->result_ready.wait();
        if (state->error) {
            state->error_observed = true;
            std::rethrow_exception(state->error);
        }
        if (state->plugin_cancelled) {
            throw AbiCancelled{};
        }
        co_return std::move(state->result);
    } catch (const AbiCancelled&) {
        if (scope.cancelled()) {
            // Host teardown (shutdown or an outer budget) already cancelled
            // this scope: propagate the cancellation.
            scope.throw_if_cancelled();
        }
        // A driver-reported cancellation on a live scope is one failed cycle,
        // not the end of the subdomain loop: it becomes an error value and the
        // schedule retries on the next interval.
        SPDLOG_WARN(R"(Driver '{}' reported cancellation for "{}"; the cycle fails and retries on schedule)",
                    driver_name, fqdn);
        co_return std::unexpected(
            domain::DriverError{domain::DriverError::Code::UPDATE_FAILED,
                                fmt::format("Driver '{}' reported cancellation for {}", driver_name, fqdn), 0});
    } catch (const coro::Cancelled&) {
        state->request_cancel();
        throw;
    }
}

bool DriverGateway::supports_validate(std::string_view driver_name) const {
    const auto module = catalog_.find(driver_name);
    return module != nullptr && module->supports_validate();
}

coro::Task<std::expected<void, domain::DriverError>> DriverGateway::validate_config(std::string driver_name,
                                                                                    std::string driver_param_json) {
    auto module = catalog_.find(driver_name);
    if (module == nullptr) {
        co_return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND,
                                                      fmt::format("Driver '{}' is not loaded", driver_name)});
    }

    // OPTIONAL entry (optional since ABI 1.0). The plugin still loads.
    // config test cannot confirm driver_params without the entry.
    if (!module->supports_validate()) {
        co_return std::unexpected(domain::DriverError{
            domain::DriverError::Code::UNKNOWN,
            fmt::format("Driver '{}' does not provide yaddnsc_driver_validate; its configuration was not checked",
                        driver_name),
            0});
    }

    auto state = std::make_shared<CallState>();
    auto bridge = bridge_;
    const app::LoggerPort* const logger = &logger_;
    coro::CancelScope& scope = co_await coro::current_scope();
    scope.throw_if_cancelled();
    const coro::CancelObserver cancellation{scope, state.get(), &request_cancel_hook};
    const CycleSubscription subscription{*state};
    bridge->start_cycle(state, make_cycle_owner(state,
                                                [module, param = std::move(driver_param_json), bridge, logger,
                                                 state]() mutable -> std::expected<void, domain::DriverError> {
                                                    return run_validate_cycle(module, param, *bridge, *logger, state);
                                                }));
    try {
        co_await state->result_ready.wait();
        if (state->error) {
            state->error_observed = true;
            std::rethrow_exception(state->error);
        }
        if (state->plugin_cancelled) {
            throw AbiCancelled{};
        }
        co_return std::move(state->result);
    } catch (const AbiCancelled&) {
        // A foreign status is not a host cancellation request. Only an already
        // cancelled host scope propagates control flow; live-scope validation
        // failure must reach the config-test verification boundary as a value.
        scope.throw_if_cancelled();
        co_return std::unexpected(domain::DriverError{
            domain::DriverError::Code::UNKNOWN,
            fmt::format("Driver '{}' reported cancellation during configuration validation", driver_name), 0});
    } catch (const coro::Cancelled&) {
        state->request_cancel();
        throw;
    }
}

}  // namespace plugin
