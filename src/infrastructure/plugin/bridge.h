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
//          future.wait_until(deadline)    serve(call)  [structured child of
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
// Cancellation and lifetime: gateway cycles own a shielded group containing
// joined worker work and all live/retiring HTTP children. The root owns the
// cycle, and an independent loop controller handles cancellation while the
// worker waits. Sticky cycle cancellation participates in loop admission, so
// a late worker publication cannot start an exchange after caller cancellation.
// Standalone Bridge calls use the supplied root group directly.
//
// Admission uses a shared group lifetime token, checked only on the loop.
// Requests posted after group closure are rejected without dereferencing a dead
// group. Dispatch/start failures fulfil the promise instead of escaping the
// loop callback. Worker waiting uses an absolute steady-clock deadline,
// including queueing. At admission its remaining duration is translated to a
// loop-clock deadline; worker expiry requests actual exchange cancel. A manual
// loop clock does not make real workers or sockets deterministic.
// There is no slack-based assumption that loop timer processing happens first.
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_BRIDGE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_BRIDGE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/host.hpp"
#include "coro/notification.hpp"
#include "coro/task.hpp"
#include "domain/error/error.h"
#include "infrastructure/http/types.h"

namespace coro {
class Loop;
class TaskGroup;
class CancelScope;
}  // namespace coro

namespace plugin {

/// Coarse host-service ABI result; cancellation stays a value for the worker.
struct BridgeError {
    yaddnsc_status code;
    std::string message;
    uint32_t retry_after_seconds = 0;
};

struct CallState;

/// One bridged exchange: everything the loop side needs, plus the shared cell
/// the worker waits on.
///
/// Ownership: held by std::shared_ptr; the worker, the posted spawn closure and
/// the loop-side coroutine each hold a share, so an abandoned wait never leaves
/// the loop side with a dangling promise. cycle optionally retains its owner;
/// the loop removes the call from that owner's weak live set on completion.
/// Thread safety: `cancelled` is atomic, worker_deadline/request are immutable after
/// posting, and the promise synchronizes result delivery. `live_scope` is
/// loop-thread only: serve_exchange publishes and clears it, and Bridge::cancel
/// reads it from a lambda posted onto the loop.
struct BridgeCall {
    std::shared_ptr<CallState> cycle;
    std::string url;
    http::Request request;
    /// Fulfilled exactly once by the loop-side coroutine, on every path.
    std::promise<std::expected<http::Response, BridgeError>> promise;
    /// Latched when the workflow abandons the call (Bridge::cancel) or the
    /// bridge scope is cancelled; read by the plugin's is_cancelled() polling.
    std::atomic<bool> cancelled{false};
    /// The cancel scope of the in-flight loop-side exchange, while it is
    /// cancellable. Loop thread only (see above); null before the exchange
    /// starts and after it leaves its scope.
    coro::CancelScope* live_scope = nullptr;
    /// Real-time boundary covering dispatch, loop I/O and worker waiting.
    /// Written before posting; immutable afterwards. Never registered directly
    /// on the loop clock: serve_exchange translates the remaining duration.
    std::chrono::steady_clock::time_point worker_deadline{};
};

/// One cycle owner: only cancelled crosses threads; all other fields are loop-only.
/// The owner and worker retain leases until HTTP children and real work finish.
struct CallState {
    std::atomic<bool> cancelled{false};
    coro::GroupToken lifetime;
    // The group owns HTTP children; tracking borrows without a cycle->call->cycle ring.
    std::vector<std::weak_ptr<BridgeCall>> calls;
    /// Owner-side events: cancel requests, worker completion, subscriber exit.
    coro::Notification owner_changed;
    /// The owner published the cycle's result (or defect). Awaiting it consumes
    /// one publication; cancellation of the subscriber's scope still wins.
    coro::Notification result_ready;
    std::expected<void, domain::DriverError> result;
    std::exception_ptr error;
    /// Worker-only until joined completion publishes it to the loop.
    std::exception_ptr worker_error;
    coro::CancelScope* worker_scope = nullptr;
    bool subscriber_active = false;
    bool error_observed = false;
    bool plugin_cancelled = false;
    bool worker_done = false;

    /// Worker-only until joined completion; preserve the first host defect.
    void record_worker_error(std::exception_ptr defect) noexcept {
        if (!worker_error) {
            worker_error = std::move(defect);
        }
    }

    /// Loop-only signal; cancellation hooks may call this without allocation.
    void request_cancel() noexcept;
};

namespace detail {

/// Immutable block shared by every exchange a Bridge starts.
///
/// Ownership: shared_ptr; this is what lets the loop-side coroutine outlive the
/// Bridge object, because it copies nothing from the (borrowed) Bridge after
/// spawn.
struct BridgeState {
    http::Options options;
    std::chrono::milliseconds wait_budget{0};
};

/// Loop side: perform the exchange and fulfil the promise on every path.
///
/// Ownership: takes a share of the state and of the call. The guard fulfils the
/// promise even when this coroutine is cancelled or reaped, so the worker never
/// blocks past the promise.
/// Failure: BridgeError carries an ABI status: cancellation is CANCELLED,
/// timeout / transport failure is NETWORK_ERROR, and defects are INTERNAL_ERROR.
/// All paths fulfil the promise so the synchronous worker can finish.
/// Thread safety: loop thread only.
[[nodiscard]] coro::Task<void> serve_exchange(std::shared_ptr<const BridgeState> state,
                                              std::shared_ptr<BridgeCall> call);

}  // namespace detail

/// Carries host-service HTTP requests from a worker to the loop.
///
/// Ownership: borrows the loop, which must outlive the bridge and its callers.
/// A shared loop-thread lifetime token closes group ingress before exit drain;
/// the bridge may outlive the group, but then exchanges are rejected. Workers
/// hold their Bridge share through exchange(); request coroutines hold immutable
/// policy/call state independently. Submitted offloads finish before run returns.
/// Failure: BridgeError carries the existing ABI status. A cancelled
/// call/closed group is CANCELLED; deadline expiry or recoverable HTTP failure
/// is NETWORK_ERROR. Defects are INTERNAL_ERROR.
/// Thread safety: exchange() may be called from any worker thread; the loop-side
/// coroutine and the spawn group are loop-thread only.
class Bridge {
public:
    /// @param loop          Loop that runs the exchanges.
    /// @param spawn_group   Structured scope for the loop-side coroutines; the
    ///                      application binds its own root group here so bridge
    ///                      work is never a detached task.
    /// @param options       HTTP policy for the loop-side client.
    /// @param wait_budget   Real-time budget for admission and I/O. The loop
    ///                      translates the remaining budget to its own clock;
    ///                      worker expiry cancels the loop-side request.
    Bridge(coro::Loop& loop, coro::TaskGroup& spawn_group, http::Options options,
           std::chrono::milliseconds wait_budget);

    ~Bridge();

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    Bridge(Bridge&&) = delete;
    Bridge& operator=(Bridge&&) = delete;

    /// Start an owned cycle in the long-lived root group. Loop thread only.
    /// `state` must outlive the cycle; a submission failure is published to the
    /// subscriber through the allocation-free notification before the error
    /// propagates, so no caller can hang on a cycle that never started.
    /// Allocation may throw; closed ingress throws std::logic_error.
    void start_cycle(const std::shared_ptr<CallState>& state, coro::Task<void> cycle);

    /// Worker side: run `call` on the loop and return its answer. Blocks the
    /// calling thread; may be called from any thread.
    [[nodiscard]] std::expected<http::Response, BridgeError> exchange(std::shared_ptr<BridgeCall> call);

    /// Abort the in-flight loop-side exchange of `call`, if any: the call's
    /// own cancel scope is cancelled on the loop, so the HTTP await fails fast
    /// and the blocked worker is released with a cancellation instead of
    /// running the exchange to completion. Idempotent; a null or finished call
    /// is a no-op. Safe to call from any thread.
    void cancel(std::shared_ptr<BridgeCall> call) noexcept;

private:
    coro::Loop* loop_;
    coro::GroupToken group_lifetime_;
    std::shared_ptr<const detail::BridgeState> state_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_BRIDGE_H
