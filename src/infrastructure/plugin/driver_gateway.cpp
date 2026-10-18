//
// plugin — the coroutine driver gateway over the v1 C ABI plugin host
// (implementation).
//

#include "driver_gateway.h"

#include <algorithm>
#include <cassert>
#include <atomic>
#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
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
#include "coro/now.hpp"
#include "coro/nursery.hpp"
#include "coro/offload.hpp"
#include "coro/scope.hpp"
#include "coro/task.hpp"
#include "coro/time.h"
#include "coro/with_nursery.hpp"
#include "domain/error/error.h"
#include "domain/update/driver_update_command.h"
#include "infrastructure/http/client.h"
#include "infrastructure/http/wire_request.h"
#include "infrastructure/plugin/abi_string.h"
#include "infrastructure/plugin/async_services.h"
#include "infrastructure/plugin/async_cleanup.h"
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
    const auto services = context.make_services_v1_1();

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    const yaddnsc_status create_status = module->create(services.base, &handle, error);
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
    const auto services = context.make_services_v1_1();

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    const yaddnsc_status create_status = module->create(services.base, &handle, error);
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


// ── ABI 1.1 async cycle ─────────────────────────────────────────────────────
//
// The async protocol runs on the loop. Plugin entries still execute on an
// offload worker, one shielded step at a time; accepted HTTP commands are
// applied only after that step returns, and the loop-side HTTP tasks deliver
// completions through the cycle notification. Waiting for HTTP therefore
// occupies no worker.

struct AsyncStepOutcome {
    yaddnsc_status status = YADDNSC_STATUS_INTERNAL_ERROR;
    std::string message;
    uint32_t retry_after_seconds = 0;
    bool error_valid = false;
};

struct AsyncEventPacket {
    yaddnsc_event event{};
    yaddnsc_http_response response{};
    yaddnsc_error error{};
    std::vector<std::pair<std::string, std::string>> header_storage;
    std::vector<yaddnsc_http_header> headers;
    std::string body;
    std::string error_message;

    AsyncEventPacket() {
        event.struct_size = static_cast<uint32_t>(sizeof(event));
        event.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
        event.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    }
};

struct AsyncOp {
    uint64_t id = 0;
    std::string url;
    http::Request request;
    coro::CancelScope* scope = nullptr;
    bool ready = false;
    bool canceled = false;
};

struct AsyncResources {
    std::shared_ptr<CallState> state;
    std::shared_ptr<const PluginModule> module;
    domain::DriverUpdateCommand command;
    const app::LoggerPort* logger;
    http::Options http;
    std::chrono::milliseconds budget;
    std::unique_ptr<HostAsyncServicesContext> services;
    AsyncStepCommands commands;
    std::deque<std::shared_ptr<AsyncEventPacket>> completions;
    std::map<uint64_t, std::shared_ptr<AsyncOp>> ops;
    std::size_t active_tasks = 0;
    bool stopping = false;
    yaddnsc_async_driver* handle = nullptr;

    AsyncResources(std::shared_ptr<CallState> call_state, std::shared_ptr<const PluginModule> driver_module,
                   domain::DriverUpdateCommand update_command, const app::LoggerPort& log_port, http::Options options,
                   std::chrono::milliseconds exchange_budget)
        : state(std::move(call_state)), module(std::move(driver_module)), command(std::move(update_command)),
          logger(&log_port), http(std::move(options)), budget(exchange_budget),
          services(std::make_unique<HostAsyncServicesContext>(log_port, state)) {}
};

using AsyncResourcePtr = std::shared_ptr<AsyncResources>;

[[nodiscard]] std::shared_ptr<AsyncEventPacket> make_http_event(uint64_t op_id, const http::Response& response) {
    auto packet = std::make_shared<AsyncEventPacket>();
    packet->response.struct_size = static_cast<uint32_t>(sizeof(packet->response));
    packet->response.status_code = static_cast<uint32_t>(response.status_);
    packet->header_storage.reserve(response.headers_.size());
    packet->headers.reserve(response.headers_.size());
    for (const auto& [name, value] : response.headers_) {
        packet->header_storage.emplace_back(name, value);
    }
    for (const auto& [name, value] : packet->header_storage) {
        packet->headers.push_back({{name.data(), name.size()}, {value.data(), value.size()}});
    }
    packet->body = std::string(response.text());
    packet->response.headers = packet->headers.empty() ? nullptr : packet->headers.data();
    packet->response.header_count = packet->headers.size();
    packet->response.body = {reinterpret_cast<const uint8_t*>(packet->body.data()), packet->body.size()};
    packet->event.kind = YADDNSC_EVENT_HTTP_FINISHED;
    packet->event.op_id = op_id;
    packet->event.status = YADDNSC_STATUS_OK;
    packet->event.response = &packet->response;
    packet->event.error = nullptr;
    return packet;
}

[[nodiscard]] std::shared_ptr<AsyncEventPacket> make_error_event(uint64_t op_id, yaddnsc_status status,
                                                                 std::string message,
                                                                 uint32_t retry_after_seconds) {
    auto packet = std::make_shared<AsyncEventPacket>();
    packet->error_message = std::move(message);
    packet->error.struct_size = static_cast<uint32_t>(sizeof(packet->error));
    packet->error.status = status;
    packet->error.retry_after_seconds = retry_after_seconds;
    packet->error.message = {packet->error_message.data(), packet->error_message.size()};
    packet->event.kind = YADDNSC_EVENT_HTTP_FINISHED;
    packet->event.op_id = op_id;
    packet->event.status = status;
    packet->event.response = nullptr;
    packet->event.error = &packet->error;
    return packet;
}

[[nodiscard]] std::shared_ptr<AsyncEventPacket> make_cancel_event() {
    auto packet = std::make_shared<AsyncEventPacket>();
    packet->error_message = "update cancelled";
    packet->error.struct_size = static_cast<uint32_t>(sizeof(packet->error));
    packet->error.status = YADDNSC_STATUS_CANCELLED;
    packet->error.message = {packet->error_message.data(), packet->error_message.size()};
    packet->event.kind = YADDNSC_EVENT_UPDATE_CANCELLED;
    packet->event.op_id = 0;
    packet->event.status = YADDNSC_STATUS_CANCELLED;
    packet->event.response = nullptr;
    packet->event.error = &packet->error;
    return packet;
}

[[nodiscard]] AsyncStepOutcome make_step_outcome(yaddnsc_status status, const yaddnsc_error& error) {
    AsyncStepOutcome outcome;
    outcome.status = status;
    if (status == YADDNSC_STATUS_OK || status == YADDNSC_STATUS_PENDING) {
        return outcome;
    }
    if (yaddnsc_status_is_valid(status) && error.struct_size >= YADDNSC_ERROR_MIN_SIZE && error.status == status &&
        yaddnsc_string_is_valid(error.message)) {
        outcome.message.assign(error.message.data, error.message.size);
        outcome.retry_after_seconds = error.retry_after_seconds;
        outcome.error_valid = true;
    }
    return outcome;
}

bool finish_async_step(AsyncResources& res, const AsyncStepOutcome& outcome, std::string_view entry) {
    if (res.state->cancelled.load(std::memory_order_acquire)) {
        res.state->plugin_cancelled = true;
        return false;
    }
    if (outcome.status == YADDNSC_STATUS_PENDING || !yaddnsc_async_step_status_is_valid(outcome.status)) {
        res.state->result = std::unexpected(invalid_plugin_result(res.module->descriptor().name, entry));
        return false;
    }
    if (outcome.status == YADDNSC_STATUS_OK) {
        return true;
    }
    if (outcome.status == YADDNSC_STATUS_CANCELLED) {
        res.state->plugin_cancelled = true;
        return false;
    }
    if (!outcome.error_valid) {
        res.state->result = std::unexpected(invalid_plugin_result(res.module->descriptor().name, entry));
        return false;
    }
    res.state->result = std::unexpected(
        map_error(outcome.status, outcome.message, res.module->descriptor().name, res.command.fqdn));
    return false;
}

template<typename Fn>
coro::Task<AsyncStepOutcome> run_async_step(Fn fn) {
    auto outcome = co_await coro::non_cancellable(
        [fn = std::move(fn)](coro::CancelScope&) mutable -> coro::Task<AsyncStepOutcome> {
            co_return co_await coro::offload_joined(std::move(fn));
        });
    assert(outcome.has_value() && "shielded async step produced no value");
    co_return std::move(*outcome);
}

[[nodiscard]] AsyncStepOutcome create_async_step(AsyncResources& res) {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    res.commands.clear();
    res.services->set_phase(HostAsyncServicesContext::Phase::CREATE);
    res.services->set_commands(&res.commands);
    yaddnsc_async_driver* handle = nullptr;
    const yaddnsc_status status = res.module->async_create(res.services->services(), &handle, error);
    res.services->clear_commands();
    res.handle = status == YADDNSC_STATUS_OK ? handle : nullptr;
    return make_step_outcome(status, error);
}

[[nodiscard]] AsyncStepOutcome begin_async_step(AsyncResources& res) {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const auto& params = res.command;
    const yaddnsc_update_request request{
        .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_update_request)),
        .ip_address = {params.ip_addr.data(), params.ip_addr.size()},
        .record_type = {params.rd_type.data(), params.rd_type.size()},
        .domain = {params.domain.data(), params.domain.size()},
        .subdomain = {params.subdomain.data(), params.subdomain.size()},
        .fqdn = {params.fqdn.data(), params.fqdn.size()},
        .driver_param_json = {reinterpret_cast<const uint8_t*>(params.driver_params.data()),
                              params.driver_params.size()},
    };
    res.commands.clear();
    res.services->set_phase(HostAsyncServicesContext::Phase::BEGIN);
    res.services->set_commands(&res.commands);
    const yaddnsc_status status = res.module->async_begin(res.handle, request, error);
    res.services->clear_commands();
    return make_step_outcome(status, error);
}

[[nodiscard]] AsyncStepOutcome resume_async_step(AsyncResources& res, std::shared_ptr<AsyncEventPacket> event) {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    res.commands.clear();
    res.services->set_phase(HostAsyncServicesContext::Phase::RESUME);
    res.services->set_commands(&res.commands);
    const yaddnsc_status status = res.module->async_resume(res.handle, event->event, error);
    res.services->clear_commands();
    return make_step_outcome(status, error);
}

[[nodiscard]] AsyncStepOutcome destroy_async_step(AsyncResources& res) {
    res.services->set_phase(HostAsyncServicesContext::Phase::DESTROY);
    res.module->async_destroy(res.handle);
    res.handle = nullptr;
    return make_step_outcome(YADDNSC_STATUS_OK, yaddnsc_error{});
}

void cancel_all_async_ops(AsyncResources& res) {
    for (auto& [id, op] : res.ops) {
        if (op->ready) {
            continue;
        }
        if (op->scope != nullptr) {
            op->scope->cancel();
        } else {
            op->canceled = true;
        }
    }
}

coro::Task<void> run_async_http(AsyncResourcePtr resource, std::shared_ptr<AsyncOp> op);

void apply_async_commands(AsyncResourcePtr resource, coro::Nursery& http_group, bool terminal) {
    AsyncResources& res = *resource;
    if (terminal) {
        res.stopping = true;
        cancel_all_async_ops(res);
        for (auto& start : res.commands.starts) {
            res.services->mark_delivered(start.op_id);
        }
        res.commands.clear();
        return;
    }

    for (const uint64_t op_id : res.commands.cancels) {
        const auto it = res.ops.find(op_id);
        if (it == res.ops.end() || it->second->ready) {
            continue;  // already delivered or READY: a late cancel cannot overwrite it
        }
        if (it->second->scope != nullptr) {
            it->second->scope->cancel();
        } else {
            it->second->canceled = true;
        }
    }

    for (auto& start : res.commands.starts) {
        if (std::find(res.commands.cancels.begin(), res.commands.cancels.end(), start.op_id) !=
            res.commands.cancels.end()) {
            res.services->mark_delivered(start.op_id);
            res.completions.push_back(
                make_error_event(start.op_id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0));
            continue;
        }
        auto op = std::make_shared<AsyncOp>();
        op->id = start.op_id;
        op->url = std::move(start.url);
        op->request = std::move(start.request);
        res.ops.emplace(op->id, op);
        ++res.active_tasks;
        try {
            http_group.spawn_discard(run_async_http(resource, op));
        } catch (...) {
            --res.active_tasks;
            throw;
        }
    }
    res.commands.clear();
}

coro::Task<void> run_async_http(AsyncResourcePtr resource, std::shared_ptr<AsyncOp> op) {
    AsyncResources& res = *resource;
    struct ActiveTaskGuard {
        AsyncResources& res;
        ~ActiveTaskGuard() {
            if (res.active_tasks != 0) {
                --res.active_tasks;
            }
            res.state->owner_changed.notify();
        }
    } guard{res};
    std::shared_ptr<AsyncEventPacket> packet;
    try {
        if (res.stopping || op->canceled || res.state->cancelled.load(std::memory_order_acquire)) {
            packet = make_error_event(op->id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0);
        } else {
        auto scoped = co_await coro::with_cancel_scope(
            [&res, op](coro::CancelScope& scope) -> coro::Task<std::expected<http::Response, http::Error>> {
                op->scope = &scope;
                struct ScopeBorrow {
                    AsyncOp& op;
                    ~ScopeBorrow() noexcept { op.scope = nullptr; }
                } borrow{*op};
                if (res.stopping || op->canceled || res.state->cancelled.load(std::memory_order_acquire)) {
                    scope.cancel();
                }
                scope.throw_if_cancelled();
                const auto deadline = co_await coro::current_time() + res.budget;
                auto outcome = co_await coro::with_deadline(
                    deadline, [&res, op]() -> coro::Task<std::expected<http::Response, http::Error>> {
                        http::Client client{res.http};
                        co_return co_await client.exchange(op->url, op->request);
                    });
                if (outcome.timed_out) {
                    co_return std::unexpected(
                        http::Error{http::ErrorCode::QUEUE_TIMEOUT, "the API server did not respond in time", 0});
                }
                if (outcome.cancelled) {
                    co_return std::unexpected(
                        http::Error{http::ErrorCode::CONNECTION_LOST, "the request was cancelled", 0});
                }
                co_return std::move(*outcome);
            });

        if (scoped.cancelled) {
            packet = make_error_event(op->id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0);
        } else if (scoped.timed_out) {
            packet = make_error_event(op->id, YADDNSC_STATUS_NETWORK_ERROR,
                                      "the API server did not respond in time", 0);
        } else if (!scoped.has_value()) {
            packet = make_error_event(op->id, YADDNSC_STATUS_INTERNAL_ERROR, "async HTTP task produced no result", 0);
        } else if (*scoped) {
            packet = make_http_event(op->id, **scoped);
        } else {
            const auto& error = (*scoped).error();
            packet = make_error_event(op->id, YADDNSC_STATUS_NETWORK_ERROR,
                                      http::failure_reason("API server", error), error.retry_after_seconds);
        }
        }
    } catch (const coro::Cancelled&) {
        packet = make_error_event(op->id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0);
    } catch (...) {
        if (!res.state->error) {
            res.state->error = std::current_exception();
        }
        res.state->owner_changed.notify();
        co_return;
    }
    try {
        op->ready = true;
        res.completions.push_back(std::move(packet));
    } catch (...) {
        if (!res.state->error) {
            res.state->error = std::current_exception();
        }
    }
    res.state->owner_changed.notify();
    co_return;
}

/// Loop-side cancellation watcher. The protocol coroutine can be waiting for a
/// shielded worker step and cannot process owner_changed itself; this sibling
/// runs on the loop, observes sticky cancellation, and stops in-flight HTTP
/// immediately. The step still has to join before destroy (it may be running
/// plugin code), but network work no longer waits for it.
coro::Task<void> watch_async_cancellation(AsyncResourcePtr resource) {
    AsyncResources& res = *resource;
    while (!res.stopping && !res.state->error) {
        if (res.state->cancelled.load(std::memory_order_acquire)) {
            res.stopping = true;
            cancel_all_async_ops(res);
            res.state->owner_changed.notify();
            co_return;
        }
        try {
            co_await res.state->cancel_changed.wait();
        } catch (const coro::Cancelled&) {
            co_return;
        }
    }
}

coro::Task<void> async_cycle_body(AsyncResourcePtr resource) {
    AsyncResources& res = *resource;
    try {
        if (res.state->cancelled.load(std::memory_order_acquire)) {
            res.state->plugin_cancelled = true;
            co_return;
        }

        co_await coro::with_nursery(
            resource,
            [](coro::Nursery& http_group, AsyncResourcePtr& shared) -> coro::Task<void> {
                AsyncResources& r = *shared;
                struct StopGuard {
                    AsyncResources& r;
                    ~StopGuard() {
                        // Make normal early returns and exceptions release the
                        // watcher and stop any HTTP that is still in flight.
                        r.stopping = true;
                        cancel_all_async_ops(r);
                        r.state->owner_changed.notify();
                        r.state->cancel_changed.notify();
                    }
                } stop_guard{r};
                http_group.spawn_discard(watch_async_cancellation(shared));

                auto create = co_await run_async_step([&r] { return create_async_step(r); });
                if (!finish_async_step(r, create, "async create")) {
                    co_return;
                }
                if (r.state->cancelled.load(std::memory_order_acquire)) {
                    r.state->plugin_cancelled = true;
                    co_return;
                }

                auto begin = co_await run_async_step([&r] { return begin_async_step(r); });
                if (begin.status != YADDNSC_STATUS_PENDING) {
                    apply_async_commands(shared, http_group, true);
                    finish_async_step(r, begin, "async begin");
                    co_return;
                }
                apply_async_commands(shared, http_group, false);
                if (r.ops.empty() && r.completions.empty()) {
                    r.state->result = std::unexpected(invalid_plugin_result(r.module->descriptor().name, "async begin"));
                    co_return;
                }

                while (true) {
                    if (r.state->error) {
                        break;
                    }
                    if (r.state->cancelled.load(std::memory_order_acquire)) {
                        r.stopping = true;
                        cancel_all_async_ops(r);
                        auto cancel_event = make_cancel_event();
                        auto outcome = co_await run_async_step(
                            [&r, cancel_event] { return resume_async_step(r, cancel_event); });
                        if (outcome.status == YADDNSC_STATUS_PENDING) {
                            r.state->result =
                                std::unexpected(invalid_plugin_result(r.module->descriptor().name, "async cancel"));
                        } else {
                            finish_async_step(r, outcome, "async cancel");
                        }
                        break;
                    }
                    if (!r.completions.empty()) {
                        auto event = std::move(r.completions.front());
                        r.completions.pop_front();
                        if (event->event.kind == YADDNSC_EVENT_HTTP_FINISHED) {
                            r.ops.erase(event->event.op_id);
                            r.services->mark_delivered(event->event.op_id);
                        }
                        auto outcome = co_await run_async_step(
                            [&r, event] { return resume_async_step(r, event); });
                        if (outcome.status == YADDNSC_STATUS_PENDING) {
                            if (r.state->cancelled.load(std::memory_order_acquire)) {
                                r.stopping = true;
                                cancel_all_async_ops(r);
                                auto cancel_event = make_cancel_event();
                                auto cancel_outcome = co_await run_async_step(
                                    [&r, cancel_event] { return resume_async_step(r, cancel_event); });
                                if (cancel_outcome.status == YADDNSC_STATUS_PENDING) {
                                    r.state->result = std::unexpected(
                                        invalid_plugin_result(r.module->descriptor().name, "async cancel"));
                                } else {
                                    finish_async_step(r, cancel_outcome, "async cancel");
                                }
                                break;
                            }
                            apply_async_commands(shared, http_group, false);
                            if (r.ops.empty() && r.completions.empty()) {
                                r.state->result = std::unexpected(
                                    invalid_plugin_result(r.module->descriptor().name, "async resume"));
                                break;
                            }
                            continue;
                        }
                        apply_async_commands(shared, http_group, true);
                        finish_async_step(r, outcome, "async resume");
                        break;
                    }
                    co_await r.state->owner_changed.wait();
                }

                r.stopping = true;
                cancel_all_async_ops(r);
                while (r.active_tasks != 0) {
                    co_await r.state->owner_changed.wait();
                }
                co_await run_async_step([&r] { return destroy_async_step(r); });
            },
            coro::NurseryPolicy::SUPERVISOR);
    } catch (...) {
        if (!res.state->error) {
            res.state->error = std::current_exception();
        }
    }

    if (res.handle != nullptr) {
        try {
            co_await run_async_step([&res] { return destroy_async_step(res); });
        } catch (...) {
            // A destroy step that cannot run leaves the handle unaccounted
            // for; releasing module/services would be unsafe (see async_cleanup.h).
            fatal_async_cleanup_failure(res.module->descriptor().name, "async destroy step did not complete");
        }
    }

    res.state->result_ready.notify();
    while (res.state->subscriber_active) {
        co_await res.state->owner_changed.wait();
    }
}

coro::Task<void> make_async_cycle_owner(std::shared_ptr<CallState> state, AsyncResourcePtr resource) {
    auto shielded = coro::non_cancellable(
        [body = async_cycle_body(std::move(resource))](coro::CancelScope&) mutable { return std::move(body); });
    return own_cycle(std::move(state), std::move(shielded));
}


bool finish_async_validate_step(CallState& state, std::string_view driver_name,
                                              const AsyncStepOutcome& outcome, std::string_view entry) {
    if (state.cancelled.load(std::memory_order_acquire)) {
        state.plugin_cancelled = true;
        return false;
    }
    if (outcome.status == YADDNSC_STATUS_PENDING || !yaddnsc_async_step_status_is_valid(outcome.status)) {
        state.result = std::unexpected(invalid_plugin_result(driver_name, entry));
        return false;
    }
    if (outcome.status == YADDNSC_STATUS_OK) {
        return true;
    }
    if (outcome.status == YADDNSC_STATUS_CANCELLED) {
        state.plugin_cancelled = true;
        return false;
    }
    if (!outcome.error_valid) {
        state.result = std::unexpected(invalid_plugin_result(driver_name, entry));
        return false;
    }
    state.result = std::unexpected(map_validate_error(outcome.status, outcome.message, driver_name));
    return false;
}

coro::Task<void> async_validate_body(std::shared_ptr<CallState> state, std::shared_ptr<const PluginModule> module,
                                     std::string driver_param_json, const app::LoggerPort& logger) {
    HostAsyncServicesContext services{logger, state};
    yaddnsc_async_driver* handle = nullptr;
    const std::string_view driver_name = module->descriptor().name;

    auto destroy = [&]() -> coro::Task<void> {
        if (handle != nullptr) {
            co_await run_async_step([&] {
                services.set_phase(HostAsyncServicesContext::Phase::DESTROY);
                module->async_destroy(handle);
                handle = nullptr;
                return make_step_outcome(YADDNSC_STATUS_OK, yaddnsc_error{});
            });
        }
        co_return;
    };

    try {
        if (state->cancelled.load(std::memory_order_acquire)) {
            state->plugin_cancelled = true;
        } else {
            auto create = co_await run_async_step([&] {
                yaddnsc_error error{};
                error.struct_size = static_cast<uint32_t>(sizeof(error));
                services.set_phase(HostAsyncServicesContext::Phase::CREATE);
                const yaddnsc_status status = module->async_create(services.services(), &handle, error);
                return make_step_outcome(status, error);
            });
            if (finish_async_validate_step(*state, driver_name, create, "async create")) {
                if (state->cancelled.load(std::memory_order_acquire)) {
                    state->plugin_cancelled = true;
                } else {
                    const yaddnsc_string param{driver_param_json.data(), driver_param_json.size()};
                    auto validate = co_await run_async_step([&] {
                        yaddnsc_error error{};
                        error.struct_size = static_cast<uint32_t>(sizeof(error));
                        services.set_phase(HostAsyncServicesContext::Phase::VALIDATE);
                        const yaddnsc_status status = module->async_validate(handle, param, error);
                        return make_step_outcome(status, error);
                    });
                    finish_async_validate_step(*state, driver_name, validate, "async validate");
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
            // Cleanup could not be submitted; we cannot prove the instance is
            // gone, so the fatal policy applies (see async_cleanup.h).
            fatal_async_cleanup_failure(driver_name, "async validate destroy step did not complete");
        }
    }

    // Every exit path converges here. In particular, a create failure must
    // publish its saved result instead of returning before result_ready.
    state->result_ready.notify();
    while (state->subscriber_active) {
        co_await state->owner_changed.wait();
    }
}

coro::Task<void> make_async_validate_owner(std::shared_ptr<CallState> state,
                                           std::shared_ptr<const PluginModule> module, std::string driver_param_json,
                                           const app::LoggerPort& logger) {
    auto shielded = coro::non_cancellable(
        [body = async_validate_body(state, std::move(module), std::move(driver_param_json), logger)](
            coro::CancelScope&) mutable { return std::move(body); });
    return own_cycle(std::move(state), std::move(shielded));
}

}  // namespace

DriverGateway::DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                             coro::Nursery& bridge_group)
    : DriverGateway(catalog, logger, loop, bridge_group, Options{}) {}

DriverGateway::DriverGateway(const DriverCatalog& catalog, const app::LoggerPort& logger, coro::Loop& loop,
                             coro::Nursery& bridge_group, Options options)
    : catalog_(catalog), logger_(logger), options_(std::move(options)),
      bridge_(std::make_shared<Bridge>(loop, bridge_group, options_.http, options_.bridge_wait_budget)) {}

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

    if (module->mode() == ExecutionMode::Async) {
        auto state = std::make_shared<CallState>();
        auto bridge = bridge_;
        const app::LoggerPort* const logger = &logger_;
        const std::string fqdn = command.fqdn;
        coro::CancelScope& scope = co_await coro::current_scope();
        scope.throw_if_cancelled();
        const coro::CancelObserver cancellation{scope, state.get(), &request_cancel_hook};
        const CycleSubscription subscription{*state};
        auto resource = std::make_shared<AsyncResources>(state, module, std::move(command), *logger, options_.http,
                                                         options_.bridge_wait_budget);
        bridge->start_cycle(state, make_async_cycle_owner(state, std::move(resource)));
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
                scope.throw_if_cancelled();
            }
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

    if (module->mode() == ExecutionMode::Async) {
        if (!module->supports_validate()) {
            co_return std::unexpected(domain::DriverError{
                domain::DriverError::Code::UNKNOWN,
                fmt::format("Driver '{}' does not provide yaddnsc_driver_async_validate; its configuration was not "
                            "checked",
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
        bridge->start_cycle(state, make_async_validate_owner(state, module, std::move(driver_param_json), *logger));
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
            scope.throw_if_cancelled();
            co_return std::unexpected(domain::DriverError{
                domain::DriverError::Code::UNKNOWN,
                fmt::format("Driver '{}' reported cancellation during configuration validation", driver_name), 0});
        } catch (const coro::Cancelled&) {
            state->request_cancel();
            throw;
        }
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
