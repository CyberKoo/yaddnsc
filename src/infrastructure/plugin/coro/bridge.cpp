//
// plugin — the worker ↔ loop bridge for host-service HTTP (implementation).
//

#include "bridge.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>

#include "infrastructure/coro/group.hpp"
#include "infrastructure/coro/loop.h"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/net/http/client.h"

namespace plugin {

namespace {

/// Head-room the worker waits beyond the loop-side budget, so the loop's
/// `with_timeout` always fires first and the worker observes a real error
/// instead of a bare wait timeout.
constexpr std::int64_t BRIDGE_WAIT_SLACK_MS = 250;

/// Fulfils a bridge promise exactly once, on every path.
///
/// Ownership: holds a share of the call. The destructor runs when the
/// loop-side coroutine completes *or* is reaped by its group, so the worker is
/// released even when the coroutine never reached its normal end.
struct PromiseFulfil {
    std::shared_ptr<BridgeCall> call;
    bool done = false;

    void set(std::expected<http::Response, http::Error> value) {
        if (!done) {
            call->promise.set_value(std::move(value));
            done = true;
        }
    }

    ~PromiseFulfil() noexcept {
        if (!done) {
            done = true;
            try {
                call->promise.set_value(std::unexpected(http::Error{http::ErrorCode::CANCELLED, "bridge cancelled"}));
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
    std::expected<http::Response, http::Error> result{std::unexpect,
                                                      http::Error{http::ErrorCode::CANCELLED, "bridge cancelled"}};
    try {
        http::Client client{state->options};
        auto outcome = co_await coro::with_timeout(
            state->wait_budget, [&client, &fulfil](coro::CancelScope&) -> coro::Task<std::expected<http::Response, http::Error>> {
                co_return co_await client.exchange(fulfil.call->url, fulfil.call->request);
            });
        if (outcome.timed_out || outcome.cancelled) {
            fulfil.call->cancelled.store(true, std::memory_order_release);
        }
        if (outcome.has_value()) {
            result = std::move(*outcome);
        } else {
            result = std::unexpected(http::Error{http::ErrorCode::CANCELLED, "bridge wait budget expired"});
        }
    } catch (...) {
        // A defect here (allocation or client bug) must not strand the worker:
        // the promise is fulfilled with a cancellation value and the defect is
        // swallowed, because the loop side has no caller to rethrow to.
        result = std::unexpected(http::Error{http::ErrorCode::CANCELLED, "bridge internal error"});
    }
    fulfil.set(std::move(result));
    co_return;
}

}  // namespace detail

Bridge::Bridge(coro::Loop& loop, coro::TaskGroup& spawn_group, http::Options options,
               std::chrono::milliseconds wait_budget)
    : loop_(&loop), spawn_group_(&spawn_group),
      state_(std::make_shared<const detail::BridgeState>(detail::BridgeState{std::move(options), wait_budget})) {}

Bridge::~Bridge() = default;

std::expected<http::Response, http::Error> Bridge::exchange(std::shared_ptr<BridgeCall> call) {
    if (stopped()) {
        call->cancelled.store(true, std::memory_order_release);
        return std::unexpected(http::Error{http::ErrorCode::CANCELLED, "bridge stopped"});
    }

    // Read every member before the post: from here on the exchange touches only
    // its own locals and the shared state, so the Bridge may be destroyed while
    // the worker is still blocked on an abandoned call.
    coro::Loop* const loop = loop_;
    coro::TaskGroup* const group = spawn_group_;
    const std::shared_ptr<const detail::BridgeState> state = state_;
    auto future = call->promise.get_future();

    try {
        loop->post([group, state, call] { group->spawn(detail::serve_exchange(state, call)); });
    } catch (...) {
        call->cancelled.store(true, std::memory_order_release);
        return std::unexpected(http::Error{http::ErrorCode::CANCELLED, "bridge post failed"});
    }

    const auto budget = state->wait_budget + std::chrono::milliseconds{BRIDGE_WAIT_SLACK_MS};
    if (future.wait_for(budget) != std::future_status::ready) {
        call->cancelled.store(true, std::memory_order_release);
        return std::unexpected(http::Error{http::ErrorCode::CANCELLED, "bridge wait budget expired"});
    }
    return future.get();
}

}  // namespace plugin
