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
// The self-pipe wakes poll() for cross-thread inbox delivery. StopSignals owns
// process handlers separately and uses ordinary fd readiness for stop waits.
//
// The public surface here is what a caller owns and observes: the clock, the
// stop flag, the cross-thread ingress, the offload worker count and the trace
// sink. Scheduling
// and the timer/fd registrations are private and reached through
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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <source_location>
#include <string_view>
#include <vector>

#include <poll.h>  // pollfd scratch members below; a POSIX header in a public header, accepted — the runtime is POSIX-only

#include "coro/clock.h"
#include "coro/detail/fd_entry.h"
#include "coro/time.h"
#include "support/util/fd.hpp"

namespace coro {
namespace detail {
struct PromiseBase;
struct TimerNode;
struct LoopAccess;
struct OffloadAdmission;
struct InboxNode;
}  // namespace detail

/// The single-threaded event loop plus its cross-thread ingress.
///
/// Thread safety: every member except post()/post_node() is loop-thread only —
/// calling schedule(), the timer/fd registration, or run() from another thread
/// is a data race. post() and post_node() are the only cross-thread entry
/// points; they never run the callback inline, they only enqueue it for the
/// loop thread.
///
/// Instances own independent scheduling and wake state.
class Loop {
public:
    /// Loop with an internal system clock. Throws std::runtime_error if
    /// self-pipe creation fails; allocation failures propagate.
    Loop();
    /// Loop with a caller-owned clock (manual clocks make timers deterministic).
    /// The clock must outlive the loop. Same failure contract as Loop().
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

    /// Mark root completion; run() still drains submitted workers. Loop thread only.
    /// This is not cancellation: external stop with a live root is fatal in coro::run.
    void request_stop() noexcept { stopped_ = true; }

    /// Cross-thread ingress: enqueue a callback for the loop thread.
    /// Callable from any thread; the callback itself runs on the loop thread.
    /// The callback runs inline during inbox processing, ahead of the ready
    /// queue: it must only schedule or signal, never block or do heavy work.
    /// The callback must not throw; an escaping exception is fatal dispatch failure.
    /// Allocates (one closure node), so posting may throw std::bad_alloc.
    /// Caller must ensure the loop outlives posting; submitted offload work is
    /// joined before run() returns. Interrupted wake writes retry; a full pipe
    /// already carries a wakeup. Other wake failures terminate rather than
    /// silently lose delivery.
    void post(std::function<void()> fn);

    /// Diagnostic trace sink: when set, the loop reports internal scheduling
    /// events (ready-queue drains, poll cycles, timer/fd registration,
    /// offload submissions) as pre-formatted text together with the reporting
    /// call site. The runtime
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
    /// Zero is normalized to one; capacity never uses BS hardware defaults.
    void set_offload_workers(unsigned workers) noexcept;

    /// Run until root completion and submitted-worker drain. Runs on the caller.
    void run();

private:
    friend struct detail::LoopAccess;

    /// The worker pool, defined in loop.cpp so its third-party type stops here.
    class Pool;

    /// One ready fd, snapshotted before dispatch: a callback may remove its own
    /// registration, so poll_once never walks fds_ while dispatching.
    struct ReadyEntry {
        void (*fn)(void*, short) noexcept;
        void* context;
        int fd;
        short revents;
    };

    /// Enqueue a frame for resumption on the next drain. Never throws and never
    /// resumes inline: the caller keeps running. A frame already queued is not
    /// queued twice — a duplicate wakeup carries no information.
    void schedule(detail::PromiseBase& frame) noexcept;

    /// Admission precedes submission; queued registrations are loop-owned borrows.
    void admit_offload(detail::OffloadAdmission& admission);
    void cancel_offload(detail::OffloadAdmission& admission) noexcept;
    void dispatch_offloads() noexcept;
    void submit_offload(std::function<void()> job);

    /// Arm `timer` to fire `action(context)` at `deadline`. Allocates (heap
    /// growth), so it may throw std::bad_alloc; in that case the timer is not
    /// armed. The caller owns `timer` and must remove it before it dies.
    void add_timer(detail::TimerNode& timer, TimePoint deadline, void (*action)(void*) noexcept, void* context);
    /// Disarm `timer`. Idempotent; the timer must still be alive.
    void remove_timer(detail::TimerNode& timer) noexcept;

    /// Register `entry` for poll() dispatch; `entry` carries the fd, the events
    /// and the callback. Reserves table and dispatch storage, so it may throw
    /// std::bad_alloc; in that case the entry is not linked.
    ///
    /// The caller owns `entry` and must remove it before it dies; two entries on
    /// one fd are independent registrations.
    void add_fd(detail::FdEntry& entry);
    /// Stop polling a registration made by add_fd(). Idempotent.
    void remove_fd(detail::FdEntry& entry) noexcept;

    void open_self_pipe();
    void close_self_pipe() noexcept;
    static void on_pipe_ready(void* context, short revents) noexcept;

    void defer_reap(detail::PromiseBase& frame) noexcept;
    void reap_completed() noexcept;
    /// Resumes the queued batch; returns the resumed frame count (the drain
    /// budget input that paces post-drain poll scans).
    std::size_t drain_ready();
    void process_inbox();
    /// Fires due timers against the iteration's single time view.
    void fire_timers(TimePoint now) noexcept;
    [[nodiscard]] int poll_timeout_ms();
    void poll_once(int timeout_ms);

    /// Event-source impairment: poll() failed with an operational error. Fails
    /// every registered wait once failure is sustained, then paces the next
    /// attempt; the loop itself keeps running (timers, inbox, ready queue are
    /// not poll()-dependent). Never throws.
    void on_poll_impaired(int error) noexcept;
    /// The errno of the current poll() impairment, or 0 when healthy. fd wait
    /// resolution reads this to tell a loop-level wait failure (reported to the
    /// waiter) from a socket-level POLLERR (left for the next syscall).
    [[nodiscard]] int poll_error() const noexcept { return poll_error_; }

    /// Enqueue a caller-owned inbox node from any thread; allocation-free and
    /// never throws, so completion paths cannot lose delivery. The node must
    /// not be re-posted before its run() executes (see detail/inbox_node.h).
    void post_node(detail::InboxNode& node) noexcept;

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
    detail::PromiseBase* reap_head_ = nullptr;
    // Frames resumed since the last poll; paces the post-drain poll(0) scan.
    std::size_t frames_since_poll_ = 0;

    std::vector<detail::TimerNode*> timers_;
    std::uint64_t timer_sequence_ = 0;

    // Poll-order table of borrowed registration nodes; entries are owned by
    // their awaitables. fds_revision_ bumps on every add/remove and gates the
    // pollfds_scratch_ rebuild in poll_once (poll() writes only revents, so a
    // scratch built from an unchanged table stays valid).
    std::vector<detail::FdEntry*> fds_;
    std::uint64_t fds_revision_ = 0;
    std::uint64_t poll_revision_ = 0;
    // Dispatch scratch: registration reserves at least fds_.size() slots in
    // both buffers, so snapshot construction needs no storage growth.
    std::vector<pollfd> pollfds_scratch_;
    std::vector<ReadyEntry> ready_entries_scratch_;
    detail::FdEntry pipe_entry_{};
    Utils::UniqueFd pipe_read_;
    Utils::UniqueFd pipe_write_;
    // poll() impairment state: the errno of the last failed poll (0 = healthy)
    // and the consecutive-failure count that drives shedding and pacing.
    int poll_error_ = 0;
    unsigned poll_failures_ = 0;

    // Cross-thread ingress: a lock-free producer stack. Producers push and wake
    // the self-pipe; the loop thread steals the whole stack with one exchange
    // (an empty steal is a single atomic read, no lock) and dispatches FIFO.
    std::atomic<detail::InboxNode*> inbox_head_{nullptr};


    std::unique_ptr<Pool> pool_;
    unsigned pool_workers_ = default_offload_workers();
    /// Submitted-but-unfinished worker jobs; workers count down at exit and
    /// wake the loop. This counter is the entire completion protocol — the
    /// loop re-dispatches queued admissions every iteration, so the dispatch
    /// and drain paths can never observe torn flag/counter state.
    /// dispatch_offloads() and all queue links below run on the loop thread.
    std::atomic<std::size_t> offload_pending_{0};
    detail::OffloadAdmission* offload_head_ = nullptr;
    detail::OffloadAdmission* offload_tail_ = nullptr;

    TraceSink trace_sink_ = nullptr;
    void* trace_context_ = nullptr;

    bool stopped_ = false;
};

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_LOOP_H
