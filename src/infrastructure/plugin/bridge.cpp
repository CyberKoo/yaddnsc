//
// plugin — the worker ↔ loop bridge for host-service HTTP (implementation).
//

#include "bridge.h"

#include <atomic>
#include <chrono>
#include <compare>
#include <functional>
#include <memory>
#include <ratio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <utility>

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include <expected>
#include <spdlog/spdlog.h>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/detail/access.h"
#include "coro/detail/group_state.h"
#include "coro/loop.h"
#include "coro/now.hpp"
#include "coro/scope.hpp"
#include "coro/task_group.hpp"
#include "infrastructure/http/client.h"
#include "infrastructure/http/error.h"
#include "infrastructure/http/wire_request.h"
#include "yaddnsc/sdk/driver_abi.h"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace plugin {

namespace {

/// Fulfils a bridge promise exactly once, on every path.
///
/// Ownership: holds a share of the call. The destructor runs when the
/// loop-side coroutine completes *or* is reaped by its group, so the worker is
/// released even when the coroutine never reached its normal end.
struct PromiseFulfil {
    std::shared_ptr<BridgeCall> call;
    bool done = false;

    void set(std::expected<http::Response, BridgeError> value) {
        if (!done) {
            call->promise.set_value(std::move(value));
            done = true;
        }
    }

    ~PromiseFulfil() noexcept {
        if (call->cycle != nullptr) {
            std::erase_if(call->cycle->calls, [&](const auto& tracked) { return tracked.lock() == call; });
        }
        if (!done) {
            done = true;
            try {
                call->promise.set_value(std::unexpected(BridgeError{YADDNSC_STATUS_CANCELLED, {}}));
            } catch (...) {
                // set_value allocates for the message; a failure here cannot be
                // reported anywhere, and must not terminate.
            }
        }
    }
};

}  // namespace

namespace detail {

coro::Task<void> serve_exchange(std::shared_ptr<const BridgeState> state, std::shared_ptr<BridgeCall> call) {
    PromiseFulfil fulfil{std::move(call)};
    std::expected<http::Response, BridgeError> result{
        std::unexpect, BridgeError{YADDNSC_STATUS_CANCELLED, "the request was cancelled"}};
    try {
        // A cancel() that landed before this coroutine started (its posted
        // scope lookup found nothing to cancel) is honoured here instead of
        // starting a doomed exchange. Set from a fresh error rather than
        // moving `result`: GCC 15 at -O3 mis-reads the moved expected's union
        // storage in this coroutine frame as maybe-uninitialized.
        if (fulfil.call->cancelled.load(std::memory_order_acquire) ||
            (fulfil.call->cycle && fulfil.call->cycle->cancelled.load(std::memory_order_acquire))) {
            fulfil.set(std::unexpected(BridgeError{YADDNSC_STATUS_CANCELLED, "the request was cancelled"}));
            co_return;
        }
        (co_await coro::current_scope()).throw_if_cancelled();
        // Only durations cross clock domains. Sample loop time first so the
        // translation cannot extend the worker's real-time budget.
        const auto loop_now = co_await coro::current_time();
        const auto remaining = fulfil.call->worker_deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            fulfil.set(
                std::unexpected(BridgeError{YADDNSC_STATUS_NETWORK_ERROR, "the API server did not respond in time"}));
            co_return;
        }
        const auto loop_deadline = loop_now + remaining;
        http::Client client{state->options};
        // The exchange runs in the call's own cancel scope, published as
        // live_scope: a workflow abandon (Bridge::cancel) cancels the in-flight
        // exchange itself, not just the worker's wait on it.
        auto scoped = co_await coro::with_cancel_scope(
            [&client, &fulfil, loop_deadline](coro::CancelScope& call_scope)
                -> coro::Task<coro::ScopeOutcome<std::expected<http::Response, BridgeError>>> {
                fulfil.call->live_scope = &call_scope;
                struct ScopeBorrow {
                    BridgeCall& call;
                    ~ScopeBorrow() noexcept { call.live_scope = nullptr; }
                };
                // Clear on body unwind, before run_scoped destroys its scope
                // and before the outer continuation can be resumed later.
                const ScopeBorrow borrow{*fulfil.call};
                // Cancellation can land after the entry check but before this
                // scope is published. Its earlier inbox lookup then saw null.
                if (fulfil.call->cancelled.load(std::memory_order_acquire) ||
                    (fulfil.call->cycle && fulfil.call->cycle->cancelled.load(std::memory_order_acquire))) {
                    call_scope.cancel();
                }
                call_scope.throw_if_cancelled();
                auto outcome = co_await coro::with_deadline(
                    loop_deadline, [&client, &fulfil]() -> coro::Task<std::expected<http::Response, BridgeError>> {
                        auto response = co_await client.exchange(fulfil.call->url, fulfil.call->request);
                        if (!response) {
                            auto& error = response.error();
                            SPDLOG_DEBUG(R"(Driver HTTP request to "{}" failed: {})", fulfil.call->url, error.message);
                            co_return std::unexpected(BridgeError{YADDNSC_STATUS_NETWORK_ERROR,
                                                                  http::failure_reason("API server", error),
                                                                  error.retry_after_seconds});
                        }
                        co_return std::move(*response);
                    });
                co_return outcome;
            });
        if (scoped.cancelled) {
            fulfil.call->cancelled.store(true, std::memory_order_release);
            result = std::unexpected(BridgeError{YADDNSC_STATUS_CANCELLED, "the request was cancelled"});
        } else {
            auto& outcome = *scoped;
            if (outcome.timed_out) {
                result = std::unexpected(
                    BridgeError{YADDNSC_STATUS_NETWORK_ERROR, "the API server did not respond in time"});
            } else if (outcome.cancelled) {
                fulfil.call->cancelled.store(true, std::memory_order_release);
                result = std::unexpected(BridgeError{YADDNSC_STATUS_CANCELLED, "the request was cancelled"});
            } else {
                result = std::move(*outcome);
            }
        }
    } catch (const coro::Cancelled&) {
        fulfil.call->live_scope = nullptr;
        fulfil.call->cancelled.store(true, std::memory_order_release);
        result = std::unexpected(BridgeError{YADDNSC_STATUS_CANCELLED, "the request was cancelled"});
    } catch (...) {
        // Preserve host defects for the cycle's result/root error boundary.
        if (!fulfil.call->cycle) {
            fulfil.set(std::unexpected(BridgeError{YADDNSC_STATUS_INTERNAL_ERROR, {}}));
            throw;  // direct Bridge users retain their group's defect boundary
        }
        if (fulfil.call->cycle && !fulfil.call->cycle->error) {
            fulfil.call->cycle->error = std::current_exception();
        }
        result =
            std::unexpected(BridgeError{YADDNSC_STATUS_INTERNAL_ERROR, "the request failed due to an internal error"});
    }
    fulfil.call->live_scope = nullptr;
    fulfil.set(std::move(result));
    co_return;
}

}  // namespace detail

Bridge::Bridge(coro::Loop& loop, coro::TaskGroup& spawn_group, http::Options options,
               std::chrono::milliseconds wait_budget)
    : loop_(&loop), group_lifetime_(coro::detail::GroupAccess::lifetime(spawn_group)),
      state_(std::make_shared<const detail::BridgeState>(detail::BridgeState{std::move(options), wait_budget})) {}

Bridge::~Bridge() = default;

void CallState::signal() noexcept {
    if (auto* waiter = std::exchange(control, nullptr)) {
        coro::detail::wake(*waiter);
    }
}

void CallState::request_cancel() noexcept {
    cancelled.store(true, std::memory_order_release);
    signal();
}

void Bridge::start_cycle(coro::Task<void> cycle) {
    auto* group = group_lifetime_->group;
    if (group == nullptr) {
        throw std::logic_error("plugin cycle requires a live root group");
    }
    group->spawn_discard(std::move(cycle));
}

// A worker retains its own lease independently of the caller and posted children.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
std::expected<http::Response, BridgeError> Bridge::exchange(std::shared_ptr<BridgeCall> call) {
    // Read every member before the post: from here on the exchange touches only
    // its own locals and the shared state, so the Bridge may be destroyed while
    // the worker is still blocked on an abandoned call.
    coro::Loop* const loop = loop_;
    const auto lifetime = group_lifetime_;
    const std::shared_ptr<const detail::BridgeState> state = state_;
    auto future = call->promise.get_future();
    call->worker_deadline = std::chrono::steady_clock::now() + state->wait_budget;

    try {
        // spawn_discard: the group lives for the whole daemon, so a completed
        // exchange must leave the bookkeeping at once instead of accumulating
        // its frame and slot until shutdown.
        loop->post([lifetime, state, call] {
            // Admission happens on the loop, where scope closure and posting
            // cannot race. The shared token survives the group it refers to.
            const auto owner = call->cycle ? call->cycle->lifetime : lifetime;
            auto* group = owner ? owner->group : nullptr;
            if (group == nullptr || group->scope().cancelled() || call->cancelled.load(std::memory_order_acquire) ||
                (call->cycle && call->cycle->cancelled.load(std::memory_order_acquire))) {
                call->cancelled.store(true, std::memory_order_release);
                call->promise.set_value(std::unexpected(BridgeError{YADDNSC_STATUS_CANCELLED, {}}));
                return;
            }
            try {
                if (call->cycle) {
                    call->cycle->calls.push_back(call);
                }
                group->spawn_discard(detail::serve_exchange(state, call));
            } catch (...) {
                if (call->cycle) {
                    std::erase_if(call->cycle->calls, [&](const auto& tracked) { return tracked.lock() == call; });
                    if (!call->cycle->error) {
                        call->cycle->error = std::current_exception();
                    }
                }
                // Starting a child may allocate; dispatch failure is a request
                // failure, not an exception escaping the loop callback.
                call->promise.set_value(std::unexpected(BridgeError{YADDNSC_STATUS_INTERNAL_ERROR, {}}));
            }
        });
    } catch (...) {
        if (call->cycle) {
            call->cycle->record_worker_error(std::current_exception());
        }
        return std::unexpected(BridgeError{YADDNSC_STATUS_INTERNAL_ERROR, "the request could not be dispatched"});
    }

    if (future.wait_until(call->worker_deadline) != std::future_status::ready) {
        cancel(call);
        return std::unexpected(BridgeError{YADDNSC_STATUS_NETWORK_ERROR, "the API server did not respond in time"});
    }
    return future.get();
}

void Bridge::cancel(std::shared_ptr<BridgeCall> call) noexcept {
    if (call == nullptr) {
        return;
    }
    // Latch the flag at once (any thread): a serve_exchange that has not
    // started yet sees it and never starts the exchange.
    call->cancelled.store(true, std::memory_order_release);
    try {
        // The scope itself is loop-thread state, so the cancel is delivered
        // through the inbox; a call whose exchange already finished reads as
        // null and is skipped.
        loop_->post([call = std::move(call)] {
            coro::CancelScope* const scope = call->live_scope;
            if (scope != nullptr) {
                scope->cancel();
            }
        });
    } catch (...) {
        // The post allocates; without it the in-flight exchange finishes on its
        // own wait budget, which is the pre-cancel behaviour for this one call.
    }
}

}  // namespace plugin
