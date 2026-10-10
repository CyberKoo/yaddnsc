//
// Coroutine runtime — the loop.
//
// One loop, running on the thread that called run(). It owns four structures —
// a poll() file-descriptor table, a timer heap, the ready queue and a
// cross-thread inbox — plus the offload worker pool it hands out to offload().
// Every coroutine resumption goes through the ready queue (axiom 4): a resumed
// frame never runs on a caller's stack, so stack depth stays bounded and
// scheduling stays fair.
//
// Signal delivery uses the same self-pipe as the inbox: the handler only sets a
// pending bit and writes one byte, and the loop turns that into ordinary
// coroutine resumptions.
//
// The public surface here is what a caller owns and observes: the clock, the
// stop flag, the cross-thread ingress, the offload worker count and the trace
// sink. Scheduling
// and the timer/fd/signal registrations are private and reached through
// detail::LoopAccess, because each one is an invariant of a node the runtime
// owns — a registration that outlived its node, or a resumption that skipped
// the ready queue, would break the loop rather than the caller's code.
//
// The offload pool is BS::thread_pool (rule 02, Reuse Protocol): it is already
// a bundled dependency, so the runtime reuses it instead of hand-rolling a
// worker pool. It stays behind the nested Pool declaration below, so the pool's
// type never reaches a consumer of Task — offload() submits through
// submit_offload() and only loop.cpp names BS.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_LOOP_H
#define YADDNSC_INFRASTRUCTURE_CORO_LOOP_H

#include <csignal>  // IWYU pragma: keep — saved_signals_ instantiations need a complete sigaction (GCC)
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <source_location>
#include <string_view>
#include <utility>
#include <vector>

#include "coro/clock.h"
#include "support/util/fd.hpp"
#include "coro/time.h"

namespace coro {
namespace detail {
struct PromiseBase;
struct TimerNode;
struct WaitNode;
struct LoopAccess;

using FdToken = std::uint64_t;
}  // namespace detail

/// The single-threaded event loop plus its cross-thread ingress.
///
/// Thread safety: every member except post() is loop-thread only — calling
/// schedule(), the timer/fd/signal registration, or run() from another thread is
/// a data race. post() is the single cross-thread entry point and is safe to
/// call from any thread; it never runs the callback inline, it only enqueues it
/// for the loop thread.
///
/// Lifetime: a loop outlives every task started on it. run() returns once the
/// root task has completed, at which point structured scopes guarantee that no
/// descendant frame is still parked. The destructor joins the offload pool, so
/// a job abandoned by a cancelled await may still be running; such a job only
/// touches shared state and the inbox, both of which outlive the join.
class Loop {
public:
    /// Loop with an internal system clock. Throws if self-pipe creation fails.
    Loop();
    /// Loop with a caller-owned clock (manual clocks make timers deterministic).
    /// The clock must outlive the loop. Throws if self-pipe creation fails.
    explicit Loop(Clock& clock);
    ~Loop() noexcept;

    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    /// The loop's clock. Borrowed, not owned; never null.
    [[nodiscard]] Clock& clock() noexcept { return *clock_; }

    /// Current loop-clock time.
    [[nodiscard]] TimePoint now() const noexcept { return clock_->now(); }

    /// True once the root task has completed and run() has been asked to return.
    [[nodiscard]] bool stopped() const noexcept { return stopped_; }

    /// Ask run() to return after the current iteration. Loop thread only.
    void request_stop() noexcept { stopped_ = true; }

    /// Cross-thread ingress: enqueue a callback for the loop thread.
    /// Callable from any thread; the callback itself runs on the loop thread.
    /// Allocates, so it may throw std::bad_alloc.
    void post(std::function<void()> fn);

    /// Diagnostic trace sink: when set, the loop reports internal scheduling
    /// events (ready-queue drains, poll cycles, timer/fd/signal registration)
    /// as pre-formatted text together with the reporting call site. The runtime
    /// holds no logging backend — this sink is the only way those diagnostics
    /// leave the module; composition wires it to the central logging backend.
    /// The sink runs on the loop thread, must not throw, and must not call back
    /// into the loop. nullptr (the default) disables tracing: each trace site
    /// then costs a single null check. Set before run(); loop thread only.
    using TraceSink = void (*)(void* context, std::string_view message, const std::source_location& where) noexcept;
    void set_trace_sink(TraceSink sink, void* context) noexcept;

    /// Worker count of the offload pool, creating the pool on first call.
    /// Loop thread only; the pool is owned by the loop and outlives every job,
    /// because offload jobs report back through post().
    [[nodiscard]] unsigned offload_workers();

    /// Pool size when set_offload_workers() was never called:
    /// min(hardware_concurrency(), 4), at least 2. The cap keeps the pool from
    /// scaling with core count on large hosts: offload work is blocking plugin
    /// ABI cycles and similar calls, not CPU-bound parallelism. A
    /// hardware_concurrency() of 0 (unknown) still yields the minimum.
    [[nodiscard]] static unsigned default_offload_workers() noexcept;

    /// Worker count for the lazily created pool; set before the first offload.
    void set_offload_workers(unsigned workers) noexcept;

    /// Run until the root task completes. Runs on the calling thread.
    void run();

private:
    friend struct detail::LoopAccess;

    /// The worker pool, defined in loop.cpp so its third-party type stops here.
    class Pool;

    struct FdEntry {
        detail::FdToken token = 0;
        int fd = -1;
        short events = 0;
        void (*fn)(void*, short) noexcept = nullptr;
        void* context = nullptr;
    };

    struct SignalWaiter {
        detail::PromiseBase* waiter = nullptr;
        detail::WaitNode* node = nullptr;
    };

    /// Enqueue a frame for resumption on the next drain. Never throws and never
    /// resumes inline: the caller keeps running.
    void schedule(detail::PromiseBase& frame) noexcept;

    /// Hand a job to the pool, creating it on first use. Never refuses work.
    void submit_offload(std::function<void()> job);

    /// Arm `timer` to fire `action(context)` at `deadline`. Allocates (heap
    /// growth), so it may throw std::bad_alloc; in that case the timer is not
    /// armed. The caller owns `timer` and must remove it before it dies.
    void add_timer(detail::TimerNode& timer, TimePoint deadline, void (*action)(void*) noexcept, void* context);
    /// Disarm `timer`. Idempotent; the timer must still be alive.
    void remove_timer(detail::TimerNode& timer) noexcept;

    /// Poll `fd` for `events`; `fn(context, revents)` runs on the loop thread.
    /// Allocates, so it may throw std::bad_alloc.
    ///
    /// Returns a registration token. Removal takes the token, not the fd, so
    /// two waiters on one fd (a TLS read waiting POLLIN while a write waits
    /// POLLOUT, say) each unregister themselves without disturbing the other.
    [[nodiscard]] detail::FdToken add_fd(int fd, short events, void (*fn)(void*, short) noexcept, void* context);
    /// Stop polling a registration made by add_fd(). Idempotent.
    void remove_fd(detail::FdToken token) noexcept;

    /// Park `node` for signal `sig`.
    /// Allocates; invalid signals or sigaction failure throw before registration.
    /// The caller keeps `node` alive until it disarms
    /// (the node doubles as its scope waiter).
    void arm_signal(int sig, detail::WaitNode& node);
    /// Drop a parked signal waiter. Idempotent; safe from the node's on_cancel.
    void disarm_signal(int sig, detail::WaitNode& node) noexcept;

    void open_self_pipe();
    void close_self_pipe() noexcept;
    void restore_signals() noexcept;
    static void on_pipe_ready(void* context, short revents) noexcept;

    void drain_ready();
    void process_inbox();
    bool process_signals() noexcept;
    void fire_timers() noexcept;
    [[nodiscard]] int poll_timeout_ms();
    void poll_once(int timeout_ms);

    void wake() noexcept;

    // Timer heap: an indexed binary min-heap ordered by (deadline, sequence).
    void heap_push(detail::TimerNode& timer);
    void heap_remove(detail::TimerNode& timer) noexcept;
    void heap_sift_up(std::size_t index) noexcept;
    void heap_sift_down(std::size_t index) noexcept;
    [[nodiscard]] detail::TimerNode* heap_min() const noexcept;
    [[nodiscard]] detail::TimerNode* heap_pop() noexcept;

    Clock* clock_ = nullptr;
    SystemClock system_clock_{};

    detail::PromiseBase* ready_head_ = nullptr;
    detail::PromiseBase* ready_tail_ = nullptr;

    std::vector<detail::TimerNode*> timers_;
    std::uint64_t timer_sequence_ = 0;

    std::vector<FdEntry> fds_;
    std::uint64_t fd_sequence_ = 0;
    detail::FdToken self_pipe_token_ = 0;
    Utils::UniqueFd pipe_read_;
    Utils::UniqueFd pipe_write_;

    std::mutex inbox_mutex_;
    std::deque<std::function<void()>> inbox_;

    std::vector<std::vector<SignalWaiter>> signal_waiters_;
    std::vector<std::pair<int, struct sigaction>> saved_signals_;

    std::unique_ptr<Pool> pool_;
    unsigned pool_workers_ = default_offload_workers();

    TraceSink trace_sink_ = nullptr;
    void* trace_context_ = nullptr;

    bool stopped_ = false;
};

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_LOOP_H
