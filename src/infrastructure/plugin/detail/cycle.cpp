#include "cycle.h"

#include <coroutine> // IWYU pragma: keep
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/nursery.hpp"
#include "coro/scope.hpp"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp" // IWYU pragma: keep

namespace plugin {
namespace {
void request_cancel_hook(void* context) noexcept {
    static_cast<CallState*>(context)->request_cancel();
}
}  // namespace

void CallState::request_cancel() noexcept {
    cancelled.store(true, std::memory_order_release);
    owner_changed.notify();
    cancel_changed.notify();
}

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

void start_cycle(const coro::NurseryToken& group_token, const std::shared_ptr<CallState>& state, coro::Task<void> owner) {
    auto* group = group_token.get();
    if (group == nullptr) {
        throw std::logic_error("plugin cycle requires a live root group");
    }
    try {
        group->spawn_discard(std::move(owner));
    } catch (...) {
        state->error = std::current_exception();
        state->result_ready.notify();
        throw;
    }
}

CycleSubscription::CycleSubscription(CallState& state, coro::CancelScope& scope) noexcept
    : state_(state), scope_(scope), cancellation_(scope, &state, &request_cancel_hook) {
    state_.subscriber_active = true;
}

CycleSubscription::~CycleSubscription() noexcept {
    state_.subscriber_active = false;
    state_.owner_changed.notify();
}

std::expected<void, domain::DriverError> CycleSubscription::take_result(std::string_view driver_name,
                                                                        std::optional<std::string_view> fqdn) const {
    if (state_.error) {
        state_.error_observed = true;
        std::rethrow_exception(state_.error);
    }
    if (!state_.plugin_cancelled) {
        return std::move(state_.result);
    }
    scope_.throw_if_cancelled();
    if (fqdn) {
        SPDLOG_WARN(R"(Driver '{}' reported cancellation for "{}"; the cycle fails and retries on schedule)",
                    driver_name, *fqdn);
        return std::unexpected(
            domain::DriverError{domain::DriverError::Code::UPDATE_FAILED,
                                fmt::format("Driver '{}' reported cancellation for {}", driver_name, *fqdn), 0});
    }
    return std::unexpected(domain::DriverError{
        domain::DriverError::Code::UNKNOWN,
        fmt::format("Driver '{}' reported cancellation during configuration validation", driver_name), 0});
}

}  // namespace plugin
