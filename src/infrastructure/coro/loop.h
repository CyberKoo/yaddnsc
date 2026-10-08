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
// Home of the runtime's two synchronization points: the inbox mutex (worker ->
// loop) and the offload pool's own submit queue. Nothing else in the runtime is
// shared between threads.
//
// The offload pool is BS::thread_pool (rule 02, Reuse Protocol): it is already
// a bundled dependency, so the runtime reuses it instead of hand-rolling a
// worker pool. It is included here because this header names the pool type;
// offload() is the only gateway business code gets to it.
//

#ifndef YADDNSC_CORO_LOOP_H
#define YADDNSC_CORO_LOOP_H

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "infrastructure/coro/clock.h"
#include "infrastructure/coro/fwd.h"
#include "support/util/fd.hpp"

#include "BS_thread_pool.hpp"

namespace coro {

struct WaitNode;

/// Opaque fd-registration token returned by Loop::add_fd and consumed by
/// Loop::remove_fd.
using FdToken = std::uint64_t;

/// Entry in the loop's timer heap.
///
/// Lives inside the waiting frame; the loop stores only a pointer to it, so a
/// timer must be removed (not merely abandoned) before that frame dies.
/// `action` runs on the loop thread and must not throw.
struct TimerNode {
    TimePoint deadline{};
    std::uint64_t sequence = 0;
    std::size_t heap_index = 0;
    bool in_heap = false;
    void (*action)(void*) noexcept = nullptr;
    void* context = nullptr;
};

/// The single-threaded event loop plus its cross-thread ingress.
///
/// Thread safety: every member except post() and offload_pool() is loop-thread
/// only — calling schedule(), the timer/fd/signal registration, or run() from
/// another thread is a data race. post() is the single cross-thread entry point
/// and is safe to call from any thread; it never runs the callback inline, it
/// only enqueues it for the loop thread.
///
/// Lifetime: a loop outlives every task started on it. run() returns once the
/// root task has completed, at which point structured scopes guarantee that no
/// descendant frame is still parked. The destructor joins the offload pool, so
/// a job abandoned by a cancelled await may still be running; such a job only
/// touches shared state and the inbox, both of which outlive the join.
class Loop {
public:
    /// Loop with an internal system clock.
    Loop();
    /// Loop with a caller-owned clock (manual clocks make timers deterministic).
    /// The clock must outlive the loop.
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

    /// Enqueue a frame for resumption on the next drain. Loop thread only.
    /// Never throws, never resumes inline: the caller keeps running.
    void schedule(PromiseBase& frame) noexcept;

    /// Cross-thread ingress: enqueue a callback for the loop thread.
    /// Callable from any thread; the callback itself runs on the loop thread.
    /// Allocates, so it may throw std::bad_alloc.
    void post(std::function<void()> fn);

    /// Worker pool for offload(); created on first use. Loop thread only.
    /// The pool is owned by the loop and outlives every job, because offload
    /// jobs report back through post().
    [[nodiscard]] BS::thread_pool<>& offload_pool();
    /// Worker count for the lazily created pool; set before the first offload.
    void set_offload_workers(unsigned workers) noexcept;

    /// Arm `timer` to fire `action(context)` at `deadline`. Loop thread only.
    /// Allocates (heap growth), so it may throw std::bad_alloc; in that case
    /// the timer is not armed.
    void add_timer(TimerNode& timer, TimePoint deadline, void (*action)(void*) noexcept, void* context);
    /// Disarm `timer`. Idempotent; the timer must still be alive.
    void remove_timer(TimerNode& timer) noexcept;

    /// Poll `fd` for `events`; `fn(context, revents)` runs on the loop thread.
    /// Loop thread only; allocates.
    ///
    /// Returns a registration token. Removal takes the token, not the fd, so
    /// two waiters on one fd (a TLS read waiting POLLIN while a write waits
    /// POLLOUT, say) each unregister themselves without disturbing the other.
    [[nodiscard]] FdToken add_fd(int fd, short events, void (*fn)(void*, short) noexcept, void* context);
    /// Stop polling a registration made by add_fd(). Idempotent.
    void remove_fd(FdToken token) noexcept;

    /// Park `node` for signal `sig`; `*delivered` is latched when it fires.
    /// Loop thread only; allocates. The caller keeps `node` and `delivered`
    /// alive until it disarms (the node doubles as its scope waiter).
    void arm_signal(int sig, WaitNode& node, bool* delivered);
    /// Drop a parked signal waiter. Idempotent; safe from the node's on_cancel.
    void disarm_signal(int sig, WaitNode& node) noexcept;

    /// Run until the root task completes. Runs on the calling thread.
    void run();

private:
    struct FdEntry {
        FdToken token = 0;
        int fd = -1;
        short events = 0;
        void (*fn)(void*, short) noexcept = nullptr;
        void* context = nullptr;
    };

    struct SignalWaiter {
        PromiseBase* waiter = nullptr;
        WaitNode* node = nullptr;
        bool* delivered = nullptr;
    };

    void open_self_pipe();
    void close_self_pipe() noexcept;
    void restore_signals() noexcept;
    static void on_pipe_ready(void* context, short revents) noexcept;

    void drain_ready();
    bool process_inbox();
    bool process_signals() noexcept;
    bool fire_timers() noexcept;
    [[nodiscard]] int poll_timeout_ms();
    void poll_once(int timeout_ms);

    void wake() noexcept;

    // Timer heap: an indexed binary min-heap ordered by (deadline, sequence).
    void heap_push(TimerNode& timer);
    void heap_remove(TimerNode& timer) noexcept;
    void heap_sift_up(std::size_t index) noexcept;
    void heap_sift_down(std::size_t index) noexcept;
    [[nodiscard]] TimerNode* heap_min() const noexcept;
    [[nodiscard]] TimerNode* heap_pop() noexcept;

    Clock* clock_ = nullptr;
    SystemClock system_clock_{};

    PromiseBase* ready_head_ = nullptr;
    PromiseBase* ready_tail_ = nullptr;

    std::vector<TimerNode*> timers_;
    std::uint64_t timer_sequence_ = 0;

    std::vector<FdEntry> fds_;
    std::uint64_t fd_sequence_ = 0;
    FdToken self_pipe_token_ = 0;
    Utils::UniqueFd pipe_read_;
    Utils::UniqueFd pipe_write_;

    std::mutex inbox_mutex_;
    std::deque<std::function<void()>> inbox_;

    std::vector<std::vector<SignalWaiter>> signal_waiters_;
    std::vector<std::pair<int, struct sigaction>> saved_signals_;

    std::unique_ptr<BS::thread_pool<>> pool_;
    unsigned pool_workers_ = 0;

    bool stopped_ = false;
};

/// Schedule a parked frame for resumption on its own loop.
inline void wake(PromiseBase& frame) noexcept {
    if (frame.loop != nullptr) {
        frame.loop->schedule(frame);
    }
}

}  // namespace coro

#endif  // YADDNSC_CORO_LOOP_H
