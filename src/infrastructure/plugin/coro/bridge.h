//
// plugin — the worker ↔ loop bridge for host-service HTTP.
//
// A plugin entry point is synchronous C: when it calls http_exchange it must
// get an answer before it returns. The ABI call therefore runs on an offload
// worker, and this bridge carries that one request to the loop and the answer
// back:
//
//   worker (offload thread)                 loop (run() thread)
//   ----------------------                  ------------------
//   HostServicesContext::http_exchange
//     -> Bridge::exchange
//          loop.post(spawn serve)
//          future.wait_for(budget)         serve(call)  [structured child of
//          future.get()  <---- set_value <---            the bound TaskGroup]
//                                            http::Client::exchange
//
// Why a promise is allowed here (the third synchronization boundary in the
// system, after Loop::post and the pool's submit queue): the plugin C ABI is
// synchronous and may not be re-entered, so the only way to hand a result back
// across the thread boundary is to block the calling thread on shared state.
// The blocking thread is an offload worker, whose whole purpose is to block.
// Nothing else in the bridge locks, and the loop never waits on a promise: the
// promise is written on the loop and read on the worker, so the loop cannot
// deadlock against it.
//
// Cancellation and lifetime: the loop-side coroutine is spawned into a
// TaskGroup the composition root owns, so it is structured, not detached. When
// that group's scope is cancelled (shutdown), the in-flight exchange is
// cancelled at its await, the promise is fulfilled with a cancellation error,
// and the waiting worker unblocks. The coroutine shares no mutable state with
// the Bridge object: everything it needs (HTTP policy, wait budget) is copied
// into an immutable shared block at exchange() time, so an abandoned exchange
// may outlive the Bridge itself without dangling.
//
// The loop-side await additionally carries its own `with_timeout(wait_budget)`
// ceiling, so a bridge call cannot hang the worker forever even if the bound
// group's scope is never cancelled. This is the minimum acceptable cancellation
// chain for this stage: abandon (the scope cancel aborts the loop-side await and
// unblocks the worker) plus the per-exchange budget plus the shutdown scope.
// A live per-call chain that cancels the in-flight bridge exchange the instant
// the workflow's own scope is cancelled is deliberately not wired here.
//

#ifndef YADDNSC_PLUGIN_CORO_BRIDGE_H
#define YADDNSC_PLUGIN_CORO_BRIDGE_H

#include <atomic>
#include <chrono>
#include <expected>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/net/http/error.h"
#include "infrastructure/net/http/types.h"

namespace coro {
class Loop;
class TaskGroup;
}  // namespace coro

namespace plugin {

/// One bridged exchange: everything the loop side needs, plus the shared cell
/// the worker waits on.
///
/// Ownership: held by std::shared_ptr; the worker, the posted spawn closure and
/// the loop-side coroutine each hold a share, so an abandoned wait never leaves
/// the loop side with a dangling promise.
/// Thread safety: `cancelled` is the only field both sides touch, and it is
/// atomic; the promise is written once, by the loop side.
struct BridgeCall {
    std::string url;
    http::Request request;
    /// Fulfilled exactly once by the loop-side coroutine, on every path.
    std::promise<std::expected<http::Response, http::Error>> promise;
    /// Latched when the workflow abandons the call or the bridge scope is
    /// cancelled; read by the plugin's is_cancelled() polling.
    std::atomic<bool> cancelled{false};
};

/// Per-ABI-call state shared by the worker-side cycle and the plugin-facing
/// predicates.
struct CallState {
    /// Set when the enclosing scope cancelled this call (abandon).
    std::atomic<bool> cancelled{false};
    /// The exchange currently in flight for this call, if any. Worker thread
    /// only: the trampoline sets it around the blocking wait and is_cancelled()
    /// reads it from the same thread.
    std::shared_ptr<BridgeCall> in_flight;
};

namespace detail {

/// Immutable block shared by every exchange a Bridge starts.
///
/// Ownership: shared_ptr; this is what lets the loop-side coroutine outlive the
/// Bridge object, because it copies nothing from the (borrowed) Bridge after
/// spawn.
struct BridgeState {
    http::Options options;
    std::chrono::milliseconds wait_budget;
};

/// Loop side: perform the exchange and fulfil the promise on every path.
///
/// Ownership: takes a share of the state and of the call. The guard fulfils the
/// promise even when this coroutine is cancelled or reaped, so the worker never
/// blocks past the promise.
/// Failure: every failure (including cancellation and the wait budget) is
/// reported through the promise as an http::Error with code CANCELLED; a defect
/// escaping the coroutine also fulfils the promise before propagating.
/// Thread safety: loop thread only.
[[nodiscard]] coro::Task<void> serve_exchange(std::shared_ptr<const BridgeState> state,
                                              std::shared_ptr<BridgeCall> call);

}  // namespace detail

/// Carries host-service HTTP requests from a worker to the loop.
///
/// Ownership: borrows the loop and the spawn group; both must outlive the
/// bridge, and the group's scope must outlive every in-flight call. An in-flight
/// exchange does not dereference the Bridge after exchange() returns, so the
/// Bridge may be destroyed while a worker is still blocked on an abandoned call.
/// Failure: a bridge-level failure is reported as an http::Error with code
/// CANCELLED (the wait budget expired, the bridge was stopped, or the scope was
/// cancelled) so the ABI trampoline keeps one uniform mapping.
/// Thread safety: exchange() may be called from any worker thread; the loop-side
/// coroutine and the spawn group are loop-thread only. stop() is safe from any
/// thread.
class Bridge {
public:
    /// @param loop          Loop that runs the exchanges.
    /// @param spawn_group   Structured scope for the loop-side coroutines; the
    ///                      application binds its own root group here so bridge
    ///                      work is never a detached task.
    /// @param options       HTTP policy for the loop-side client.
    /// @param wait_budget   Upper bound on one exchange, enforced twice: the
    ///                      loop-side await is wrapped in it, and the worker
    ///                      waits slightly longer so the promise always wins the
    ///                      race and the worker never observes a timeout for an
    ///                      exchange the loop is still running.
    Bridge(coro::Loop& loop, coro::TaskGroup& spawn_group, http::Options options,
           std::chrono::milliseconds wait_budget);

    ~Bridge();

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    Bridge(Bridge&&) = delete;
    Bridge& operator=(Bridge&&) = delete;

    /// Worker side: run `call` on the loop and return its answer. Blocks the
    /// calling thread; may be called from any thread.
    [[nodiscard]] std::expected<http::Response, http::Error> exchange(std::shared_ptr<BridgeCall> call);

    /// Stop accepting new calls. Already-spawned exchanges finish or are
    /// cancelled with the spawn group's scope.
    void stop() noexcept { stopped_.store(true, std::memory_order_release); }

    [[nodiscard]] bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

    /// The worker's wait budget, exposed so a caller can size its own bounds.
    [[nodiscard]] std::chrono::milliseconds wait_budget() const noexcept { return state_->wait_budget; }

private:
    coro::Loop* loop_;
    coro::TaskGroup* spawn_group_;
    std::shared_ptr<const detail::BridgeState> state_;
    std::atomic<bool> stopped_{false};
};

}  // namespace plugin

#endif  // YADDNSC_PLUGIN_CORO_BRIDGE_H
