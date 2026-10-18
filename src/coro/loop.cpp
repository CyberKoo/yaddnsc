//
// Coroutine runtime — loop implementation.
//

#include "loop.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <compare>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <coroutine>
#include <cstring>  // IWYU pragma: keep — clang resolves std::strerror transitively, GCC does not
#include <ctime>    // timespec / nanosleep (POSIX names, provided in the global namespace)
#include <exception>
#include <functional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include "coro/detail/execution_record.h"
#include "coro/detail/inbox_node.h"
#include "coro/detail/offload_job.h"
#include "coro/detail/result_state.hpp"
#include "coro/detail/time_math.h"
#include "coro/detail/timer_node.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — fmt::format in the CORO_TRACE macro body

#include "BS_thread_pool.hpp"

// Trace diagnostics leave the runtime through the injected sink (loop.h);
// with no sink installed, each site costs one null check. Formatting and the
// sink call stay inside the catch-all: several sites sit in noexcept paths,
// and a diagnostic must never take the runtime down.
#define CORO_TRACE(...)                                                                                 \
    do {                                                                                                \
        if (trace_sink_ != nullptr) {                                                                   \
            try {                                                                                       \
                trace_sink_(trace_context_, fmt::format(__VA_ARGS__), std::source_location::current()); \
            } catch (...) {                                                                             \
            }                                                                                           \
        }                                                                                               \
    } while (false)

namespace coro {

namespace {

/// Offload pool sizing policy: min(hardware cores, OFFLOAD_WORKER_LIMIT), at
/// least OFFLOAD_MIN_WORKERS.
constexpr unsigned OFFLOAD_MIN_WORKERS = 2;
constexpr unsigned OFFLOAD_WORKER_LIMIT = 4;

/// Consecutive poll() failures before the loop sheds load. Below the threshold
/// a failure is absorbed invisibly: registrations stay armed and poll() is
/// level-triggered, so no readiness is lost when the next poll succeeds. At or
/// above it, every registered wait is failed with the poll errno — sustained
/// memory pressure sheds in-flight queries instead of letting them pile up.
constexpr unsigned POLL_FAILURE_SHED_THRESHOLD = 8;
/// Pacing between failed poll() attempts: 1 ms doubling, capped. nanosleep
/// allocates nothing, so it stays usable under the ENOMEM being paced.
constexpr unsigned POLL_BACKOFF_SHIFT_MAX = 6;
constexpr long long POLL_BACKOFF_CAP_MS = 50;

/// Frames drained between post-drain poll(0) scans while the ready queue keeps
/// re-filling itself; bounds I/O latency during compute-heavy stretches. A
/// skipped scan delays readiness but never loses it — poll() is level-triggered.
constexpr std::size_t DRAIN_POLL_FRAME_BUDGET = 64;

/// Render poll() event bits as a readable "POLLIN|POLLHUP" string for traces.
std::string describe_poll_events(short events) {
    std::string out;
    const auto append = [&out](const char* name) {
        if (!out.empty()) {
            out += '|';
        }
        out += name;
    };
    if ((events & POLLIN) != 0) {
        append("POLLIN");
    }
    if ((events & POLLOUT) != 0) {
        append("POLLOUT");
    }
    if ((events & POLLPRI) != 0) {
        append("POLLPRI");
    }
    if ((events & POLLERR) != 0) {
        append("POLLERR");
    }
    if ((events & POLLHUP) != 0) {
        append("POLLHUP");
    }
    if ((events & POLLNVAL) != 0) {
        append("POLLNVAL");
    }
    return out.empty() ? "0" : out;
}

/// The heap closure behind the generic post(): one node per post, destroyed
/// after it runs or at loop teardown. Offload completions ride nodes embedded
/// in their own state instead and never touch this path.
struct ClosureNode {
    detail::InboxNode node;
    std::function<void()> fn;
};

/// Runs and destroys a posted closure node (loop thread).
void run_closure_node(detail::InboxNode& node) noexcept {
    const std::unique_ptr<ClosureNode> owner{static_cast<ClosureNode*>(node.context)};
    try {
        owner->fn();
    } catch (...) {
        // post() contract: the callback must not throw. An escaping exception
        // is a fatal dispatch failure — the remaining queue cannot be unwound.
        std::terminate();
    }
}

/// Teardown hook for an orphaned closure node: destroy it without running.
void dispose_closure_node(detail::InboxNode& node) noexcept {
    const std::unique_ptr<ClosureNode> discard{static_cast<ClosureNode*>(node.context)};
}

}  // namespace

// The offload worker pool. Defined here so BS_thread_pool.hpp reaches exactly
// one translation unit: loop.h declares Pool and nothing else names BS.
//
// Placement is load-bearing: Pool must be complete before any Loop constructor
// or destructor is compiled. Unwinding out of a constructor destroys pool_,
// which instantiates ~unique_ptr<Pool>; libc++ instantiates that destructor
// eagerly and rejects an incomplete Pool, while libstdc++ happens to defer it
// and accepts it. Do not move this below the constructors.
class Loop::Pool {
public:
    explicit Pool(unsigned workers) : pool(workers) {}

    /// Fire-and-forget: the job reports back through its embedded completion
    /// node and the wrapper's accounting flag, so the pool never holds a
    /// result slot and never refuses work.
    void submit(std::function<void()> job) { pool.detach_task(std::move(job)); }

    [[nodiscard]] unsigned workers() const { return static_cast<unsigned>(pool.get_thread_count()); }

    /// Jobs still waiting for a free worker; the saturation signal for traces.
    [[nodiscard]] std::size_t queued() const { return pool.get_tasks_queued(); }

private:
    BS::thread_pool<> pool;
};

Loop::Loop() : clock_(&system_clock_) {
    open_self_pipe();
}

Loop::Loop(Clock& clock) : clock_(&clock) {
    open_self_pipe();
}

Loop::~Loop() noexcept {
    assert(!running_ && !reaping_ && "destroying loop during dispatch or retirement");
    if (running_ || reaping_) {
        std::terminate();
    }
    assert(offload_head_ == nullptr && offload_tail_ == nullptr && "destroying loop with live admission waiters");
    if (offload_head_ != nullptr || offload_tail_ != nullptr) {
        std::terminate();
    }
    // A started frame may only be destroyed by the reap transaction, and the
    // host that owns a record must outlive it. Forcing the destruction here
    // would free storage a parked frame still points into.
    const bool executions_live = !execution_registry_.empty() || !execution_ready_.empty() || !reap_candidates_.empty();
    assert(!executions_live && "destroying loop with live executions");
    if (executions_live) {
        std::terminate();
    }
    // Workers may still post after their awaits were cancelled. Join while
    // both the inbox and the wake pipe are alive.
    pool_.reset();
    // Inbox nodes queued past the last process_inbox never run: release them
    // through their dispose hooks without running any callback.
    detail::InboxNode* orphans = inbox_head_.exchange(nullptr, std::memory_order_relaxed);
    while (orphans != nullptr) {
        detail::InboxNode* next = orphans->next;
        if (orphans->dispose != nullptr) {
            orphans->dispose(*orphans);
        }
        orphans = next;
    }
    close_self_pipe();
}

void Loop::open_self_pipe() {
    auto [read_end, write_end] = Utils::make_pipe();
    if (!read_end || !write_end) {
        throw std::runtime_error("loop self-pipe creation failed");
    }
    pipe_read_ = std::move(read_end);
    pipe_write_ = std::move(write_end);
    pipe_entry_.fd = pipe_read_.get();
    pipe_entry_.events = POLLIN;
    pipe_entry_.fn = &Loop::on_pipe_ready;
    pipe_entry_.context = this;
    add_fd(pipe_entry_);
}

void Loop::close_self_pipe() noexcept {
    remove_fd(pipe_entry_);
    pipe_entry_.fd = -1;
    pipe_read_.reset();
    pipe_write_.reset();
}

void Loop::on_pipe_ready(void* context, short /*revents*/) noexcept {
    auto* self = static_cast<Loop*>(context);
    if (self->pipe_read_.get() < 0) {
        return;
    }
    char buffer[128];
    while (::read(self->pipe_read_.get(), buffer, sizeof(buffer)) > 0) {
    }
}

void Loop::wake() noexcept {
    if (pipe_write_.get() < 0) {
        return;
    }
    const char byte = 0;
    ssize_t written;
    do {
        written = ::write(pipe_write_.get(), &byte, 1);
    } while (written < 0 && errno == EINTR);
    if (written == 1 || (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        return;  // a full pipe already carries a wakeup
    }
    std::terminate();  // a queued callback must not lose its only wakeup
}

void Loop::unregister_execution(detail::ExecutionRecord& record) noexcept {
    record.registry_hook_.unlink();
}

void Loop::reap_executions() noexcept {
    // A frame destructor runs user code here. That code may add candidates,
    // post to the inbox and cancel scopes, but it must not advance the runtime:
    // a nested run/drain/reap would resume frames against a half-finished
    // transaction. New candidates are popped by this same loop instead.
    assert(!reaping_ && "recursive advance from inside the reap transaction");
    if (reaping_) {
        std::terminate();
    }
    reaping_ = true;
    while (detail::ExecutionRecord* record = reap_candidates_.pop_front()) {
        record->begin_reap();
        std::coroutine_handle<> frame = record->take_frame();
        frame.destroy();
        unregister_execution(*record);
        detail::ExecutionRecord* const parent = record->parent();
        record->detach_from_parent();
        record->mark_retired();
        // The result outlives the frame: this is what makes a value, defect or
        // cancellation still consumable after the frame is gone.
        record->result()->publish_ready();
        if (parent != nullptr) {
            // A terminated parent whose last execution child just retired
            // becomes reapable; queueing it keeps the tree from recursing.
            if (parent->phase() == detail::ExecutionPhase::TERMINAL && parent->unretired_children() == 0) {
                reap_candidates_.push_back(parent->reap_hook_);
            }
        } else {
            // The root's subtree has retired with it, so nothing may be left in
            // the registry. Legitimate late work (an abandon worker, an inbox
            // node) is not a record and never appears here.
            assert(execution_registry_.empty() && "root retired while the execution registry is not empty");
            if (!execution_registry_.empty()) {
                std::terminate();
            }
        }
        record->notify_retired();
    }
    reaping_ = false;
}

std::size_t Loop::drain_ready() {
    // Detach the batch before resuming anything: a record that queues itself
    // again (checkpoint, a due sleep) must land in the next batch, or a
    // self-refilling queue would starve poll() inside a single drain.
    detail::ForwardList<std::coroutine_handle<>> batch;
    batch.splice_all(execution_ready_);
    std::size_t resumed = 0;
    while (std::coroutine_handle<> frame = batch.pop_front()) {
        // The ready hook is unlinked before resumption. Its frame token only
        // borrows: ownership and retirement still belong to the record.
        frame.resume();
        ++resumed;
    }
    // The whole detached batch is reaped before this call returns; the design
    // requires retirement after the ready batch, not inside it.
    if (!reap_candidates_.empty()) {
        reap_executions();
    }
    CORO_TRACE("Drained ready queue, resumed {} frame(s)", resumed);
    return resumed;
}

bool Loop::session_complete() const noexcept {
    return execution_registry_.empty() && execution_ready_.empty() && reap_candidates_.empty() &&
           offload_head_ == nullptr && offload_tail_ == nullptr &&
           offload_pending_.load(std::memory_order_acquire) == 0 &&
           inbox_head_.load(std::memory_order_acquire) == nullptr;
}

void Loop::post(std::function<void()> fn) {
    auto node = std::make_unique<ClosureNode>();
    node->node.run = &run_closure_node;
    node->node.dispose = &dispose_closure_node;
    node->fn = std::move(fn);
    // Hand the allocation to the loop before it is published: the loop owns the
    // closure from here, and the inbox node outlives this call until the posted
    // work runs or teardown disposes it. Re-seeding context from the released
    // pointer keeps the handle and the node self-consistent.
    ClosureNode* const posted = node.release();
    posted->node.context = posted;
    post_node(posted->node);
}

void Loop::post_node(detail::InboxNode& node) noexcept {
    // Treiber push: link before the publishing CAS. Nodes are never re-posted
    // before running, so the stack needs no ABA guard (see detail/inbox_node.h).
    node.next = inbox_head_.load(std::memory_order_relaxed);
    while (!inbox_head_.compare_exchange_weak(node.next, &node, std::memory_order_release, std::memory_order_relaxed)) {
    }
    // Diagnostics are emitted when the inbox is consumed on the loop thread.
    wake();
}

void Loop::drain_inbox() {
    // Steal the producer stack, then reverse it for FIFO dispatch. Producers
    // racing the steal land on the next stack; their wake() byte still arrives,
    // so nothing is lost — only deferred one round.
    detail::InboxNode* stack = inbox_head_.exchange(nullptr, std::memory_order_acquire);
    detail::InboxNode* fifo = nullptr;
    std::size_t count = 0;
    while (stack != nullptr) {
        detail::InboxNode* next = stack->next;
        stack->next = fifo;
        fifo = stack;
        stack = next;
        ++count;
    }
    if (count != 0) {
        CORO_TRACE("Processing {} inbox callback(s)", count);
    }
    while (fifo != nullptr) {
        detail::InboxNode* node = fifo;
        fifo = fifo->next;
        node->run(*node);
    }
}

void Loop::cancel_offload(detail::OffloadAdmission& admission) noexcept {
    if (admission.linked) {
        if (admission.prev != nullptr) {
            admission.prev->next = admission.next;
        } else {
            offload_head_ = admission.next;
        }
        if (admission.next != nullptr) {
            admission.next->prev = admission.prev;
        } else {
            offload_tail_ = admission.prev;
        }
        admission.linked = false;
        admission.next = nullptr;
        admission.prev = nullptr;
    }
    admission.job = {};
}

void Loop::admit_offload(detail::OffloadAdmission& admission) {
    if (!admission.job || admission.failed == nullptr) {
        throw std::logic_error("offload admission requires a job and failure completion");
    }
    const unsigned capacity = offload_workers();
    if (offload_pending_ < capacity && offload_head_ == nullptr) {
        submit_offload(std::move(admission.job));
        admission.submitted = true;
        return;
    }
    admission.prev = offload_tail_;
    if (offload_tail_ != nullptr) {
        offload_tail_->next = &admission;
    } else {
        offload_head_ = &admission;
    }
    offload_tail_ = &admission;
    admission.linked = true;
}

void Loop::dispatch_offloads() noexcept {
    assert(pool_ != nullptr && "dispatch requires an initialized offload pool");
    if (!pool_) {
        std::terminate();
    }
    while (offload_head_ != nullptr && offload_pending_ < pool_->workers()) {
        auto& admission = *offload_head_;
        auto job = std::move(admission.job);
        cancel_offload(admission);
        try {
            submit_offload(std::move(job));
            admission.submitted = true;
        } catch (...) {
            // The awaiter still owns admission; failures complete it on the loop.
            admission.failed(admission.owner, std::current_exception());
        }
    }
}

void Loop::submit_offload(std::function<void()> job) {
    if (!pool_) {
        pool_ = std::make_unique<Pool>(pool_workers_);
        CORO_TRACE("Created offload pool with {} worker(s)", pool_workers_);
    }
    offload_pending_.fetch_add(1, std::memory_order_relaxed);
    try {
        // Worker-slot accounting is the loop's own concern, not the job's.
        // The countdown is the only cross-thread completion state: the loop
        // re-evaluates queued admissions every iteration, so no flag or store
        // ordering between two atomics can strand a dispatch.
        pool_->submit([this, job = std::move(job)]() mutable {
            job();
            job = {};  // release user/shared captures promptly after the run
            offload_pending_.fetch_sub(1, std::memory_order_release);
            wake();
        });
    } catch (...) {
        offload_pending_.fetch_sub(1, std::memory_order_relaxed);
        throw;
    }
    CORO_TRACE("Submitted offload job, {} now queued", pool_->queued());
}

unsigned Loop::offload_workers() {
    if (!pool_) {
        pool_ = std::make_unique<Pool>(pool_workers_);
    }
    return pool_->workers();
}

unsigned Loop::default_offload_workers() noexcept {
    return std::max(OFFLOAD_MIN_WORKERS, std::min(std::thread::hardware_concurrency(), OFFLOAD_WORKER_LIMIT));
}

void Loop::set_offload_workers(unsigned workers) noexcept {
    assert(pool_ == nullptr && "worker count must be set before the first offload");
    pool_workers_ = std::max(1U, workers);
}

void Loop::set_trace_sink(TraceSink sink, void* context) noexcept {
    trace_sink_ = sink;
    trace_context_ = context;
}

void Loop::run() {
    assert(!running_ && !reaping_ && "recursive loop advance");
    if (running_ || reaping_) {
        std::terminate();
    }
    running_ = true;

    struct ResetRunning {
        bool& running;

        ~ResetRunning() noexcept { running = false; }
    } reset{running_};

    assert(!fds_.empty() && "loop self-pipe unavailable: nothing could ever wake the loop");
    CORO_TRACE("run() starting");
    // One predicate decides when the session is over, and both the loop
    // condition and the body re-read it. Root completion is not a flag: it is
    // the registry emptying, which only happens once the root retired.
    while (!session_complete()) {
        // Worker completions and due timers run on every iteration, ahead of
        // the ready queue: a busy loop keeps re-filling the queue (every
        // resumed record can schedule the next one), so a ready-first order
        // starves all three — a with_timeout would then fire late or never.
        process_inbox();
        // Worker completions free slots without any flag or callback: queued
        // admissions are re-dispatched every iteration, ahead of the ready
        // queue. An empty queue makes this a single pointer test.
        if (offload_head_ != nullptr) {
            dispatch_offloads();
        }
        // An empty timer heap has no deadline to observe. When timers exist,
        // keep one time view and check them before dispatching ready work.
        if (!timers_.empty()) {
            fire_timers(clock_->now());
        }
        // The last batch may have retired everything it ran; re-read the same
        // predicate instead of blocking for an event that will never arrive.
        if (session_complete()) {
            break;
        }
        if (!execution_ready_.empty()) {
            frames_since_poll_ += drain_ready();
            // Keep I/O moving with a non-blocking scan, but paced: the frame
            // budget bounds the stretch a self-re-filling ready chain can run
            // without one; a changed registration table always forces a scan
            // so fresh fds are picked up promptly.
            if (frames_since_poll_ >= DRAIN_POLL_FRAME_BUDGET || poll_revision_ != fds_revision_) {
                frames_since_poll_ = 0;
                poll_once(0);
            }
            continue;
        }
        if (!reap_candidates_.empty()) {
            // A terminated execution with no ready work still has to retire;
            // nothing else would ever wake the loop for it.
            reap_executions();
            continue;
        }
        // Queued admissions hold the loop too: a worker's countdown lands
        // before its wake, so the pending count alone can read idle while
        // work still waits for a slot (~Loop terminates on live waiters).
        poll_once(poll_timeout_ms());
    }
    // A callback posted while the last batch drained — an offload completion,
    // an application callback — is still part of this session. Pump once more
    // and keep going until the same predicate holds.
    while (!session_complete()) {
        process_inbox();
        if (!execution_ready_.empty()) {
            drain_ready();
            continue;
        }
        if (!reap_candidates_.empty()) {
            reap_executions();
            continue;
        }
        if (offload_head_ != nullptr) {
            dispatch_offloads();
            continue;
        }
        poll_once(poll_timeout_ms());
    }
    CORO_TRACE("run() returning, session complete");
}

int Loop::poll_timeout_ms() {
    if (!execution_ready_.empty()) {
        return 0;
    }
    // Relaxed peek only: a post racing past it still writes the wake pipe,
    // which poll() then reports — this check exists to skip one syscall round
    // trip, not to guarantee delivery.
    if (inbox_head_.load(std::memory_order_relaxed) != nullptr) {
        return 0;
    }
    detail::TimerNode* next = heap_min();
    if (next == nullptr) {
        // A manual clock cannot be advanced by a blocking poll; return
        // immediately so the loop stays responsive to cross-thread posts and fd readiness.
        return clock_->manual() ? 0 : -1;
    }
    if (clock_->manual()) {
        clock_->jump_to(next->deadline);
    }
    const Duration remaining = detail::saturated_difference(next->deadline, clock_->now());
    if (remaining <= Duration::zero()) {
        return 0;
    }
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    if (millis <= 0) {
        return 1;
    }
    return static_cast<int>(std::min<long long>(millis, INT_MAX));
}

void Loop::poll_once(int timeout_ms) {
    if (fds_.empty()) {
        return;
    }
    // Registration reserves the scratch buffers; clear() keeps their capacity.
    // poll() writes only revents, so the fd/events array is
    // rebuilt only when the registration table changed since the last poll.
    if (poll_revision_ != fds_revision_) {
        pollfds_scratch_.clear();
        for (const detail::FdEntry* entry : fds_) {
            pollfds_scratch_.push_back(pollfd{entry->fd, entry->events, 0});
        }
        poll_revision_ = fds_revision_;
    }
    CORO_TRACE("poll() on {} fd(s), timeout {} ms", pollfds_scratch_.size(), timeout_ms);
    const int ready = ::poll(pollfds_scratch_.data(), static_cast<nfds_t>(pollfds_scratch_.size()), timeout_ms);
    if (ready < 0) {
        const int error = errno;
        if (error == EINTR) {
            return;  // reprocess inbox/timers before retrying
        }
        if (error != ENOMEM) {
            // EINVAL/EFAULT indict this call's own construction (a defect), not
            // the environment: stays on the fatal dispatch-failure path.
            throw std::system_error(error, std::generic_category(), "loop poll");
        }
        on_poll_impaired(error);
        return;
    }
    if (poll_failures_ != 0) {
        CORO_TRACE("poll() recovered after {} consecutive failure(s)", poll_failures_);
        poll_failures_ = 0;
        poll_error_ = 0;
    }
    if (ready == 0) {
        return;
    }
    CORO_TRACE("poll() reported {} ready fd(s)", ready);

    // Snapshot the ready entries before dispatching: a callback may remove its
    // own registration (or another one), which would invalidate an index walk
    // over fds_ while it is being iterated.
    ready_entries_scratch_.clear();
    for (std::size_t i = 0; i < fds_.size(); ++i) {
        if (pollfds_scratch_[i].revents != 0 && fds_[i]->fn != nullptr) {
            ready_entries_scratch_.push_back(
                ReadyEntry{fds_[i]->fn, fds_[i]->context, fds_[i]->fd, pollfds_scratch_[i].revents});
        }
    }
    // A callback may register fds and reallocate the buffer. Keep only an
    // index and a value copy across the call.
    // NOLINTNEXTLINE(modernize-loop-convert)  // reallocation hazard: not convertible to range-for
    for (std::size_t i = 0; i < ready_entries_scratch_.size(); ++i) {
        const ReadyEntry entry = ready_entries_scratch_[i];
        CORO_TRACE("Fd {} ready: {}", entry.fd, describe_poll_events(entry.revents));
        entry.fn(entry.context, entry.revents);
    }
}

void Loop::on_poll_impaired(const int error) noexcept {
    ++poll_failures_;
    poll_error_ = error;
    CORO_TRACE("poll() impaired: {} (consecutive failure {})", std::strerror(error), poll_failures_);
    if (poll_failures_ >= POLL_FAILURE_SHED_THRESHOLD) {
        // Fail every registered wait with the poll errno. Registration already
        // reserved the snapshot storage: shedding must not allocate under ENOMEM.
        // Same discipline as event dispatch: callbacks unregister themselves.
        ready_entries_scratch_.clear();
        for (const detail::FdEntry* entry : fds_) {
            if (entry->fn != nullptr) {
                ready_entries_scratch_.push_back(ReadyEntry{entry->fn, entry->context, entry->fd, POLLERR});
            }
        }
        // Callbacks may grow this buffer through add_fd(), as in poll_once().
        // NOLINTNEXTLINE(modernize-loop-convert)  // reallocation hazard: not convertible to range-for
        for (std::size_t i = 0; i < ready_entries_scratch_.size(); ++i) {
            const ReadyEntry entry = ready_entries_scratch_[i];
            entry.fn(entry.context, entry.revents);
        }
    }
    // The event source cannot pace the loop while it is itself failing, and
    // nanosleep is the remaining wait primitive that allocates nothing. A
    // signal interrupting the sleep only shortens this round's pacing.
    const unsigned shift = std::min(poll_failures_ - 1, POLL_BACKOFF_SHIFT_MAX);
    const auto millis = std::min(1LL << shift, POLL_BACKOFF_CAP_MS);
    const timespec duration{.tv_sec = millis / 1000, .tv_nsec = (millis % 1000) * 1'000'000};
    ::nanosleep(&duration, nullptr);
}

void Loop::fire_timers(const TimePoint now) noexcept {
    while (detail::TimerNode* next = heap_min()) {
        if (next->deadline > now) {
            break;
        }
        detail::TimerNode* const due = heap_pop();
        CORO_TRACE("Firing timer {}", static_cast<const void*>(due));
        if (due->action != nullptr) {
            due->action(due->context);
        }
    }
}

// ---------------------------------------------------------------------------
// Timer heap (indexed binary min-heap ordered by deadline then insertion).
// ---------------------------------------------------------------------------

void Loop::add_timer(detail::TimerNode& timer, TimePoint deadline, void (*action)(void*) noexcept, void* context) {
    assert(!timer.in_heap && "timer already armed");
    timer.deadline = deadline;
    timer.sequence = timer_sequence_++;
    timer.action = action;
    timer.context = context;
    heap_push(timer);
    CORO_TRACE(
        "Armed timer {} (fires in {} ms)", static_cast<const void*>(&timer),
        std::chrono::duration_cast<std::chrono::milliseconds>(detail::saturated_difference(deadline, now())).count());
}

void Loop::remove_timer(detail::TimerNode& timer) noexcept {
    if (timer.in_heap) {
        CORO_TRACE("Disarmed timer {}", static_cast<const void*>(&timer));
        heap_remove(timer);
    }
}

void Loop::heap_push(detail::TimerNode& timer) {
    timer.heap_index = timers_.size();
    timers_.push_back(&timer);
    timer.in_heap = true;
    heap_sift_up(timer.heap_index);
}

void Loop::heap_remove(detail::TimerNode& timer) noexcept {
    const std::size_t index = timer.heap_index;
    timer.in_heap = false;
    const std::size_t last = timers_.size() - 1;
    if (index != last) {
        timers_[index] = timers_[last];
        timers_[index]->heap_index = index;
    }
    timers_.pop_back();
    if (index < timers_.size()) {
        heap_sift_down(index);
        heap_sift_up(index);
    }
}

detail::TimerNode* Loop::heap_min() const noexcept {
    return timers_.empty() ? nullptr : timers_.front();
}

detail::TimerNode* Loop::heap_pop() noexcept {
    detail::TimerNode* top = timers_.front();
    heap_remove(*top);
    return top;
}

void Loop::heap_sift_up(std::size_t index) noexcept {
    while (index > 0) {
        const std::size_t parent = (index - 1) / 2;
        const detail::TimerNode& a = *timers_[index];
        const detail::TimerNode& b = *timers_[parent];
        if (a.deadline > b.deadline || (a.deadline == b.deadline && a.sequence >= b.sequence)) {
            break;
        }
        std::swap(timers_[index], timers_[parent]);
        timers_[index]->heap_index = index;
        timers_[parent]->heap_index = parent;
        index = parent;
    }
}

void Loop::heap_sift_down(std::size_t index) noexcept {
    const std::size_t count = timers_.size();
    for (;;) {
        const std::size_t left = 2 * index + 1;
        const std::size_t right = left + 1;
        std::size_t smallest = index;
        auto less = [this](std::size_t lhs, std::size_t rhs) {
            const detail::TimerNode& a = *timers_[lhs];
            const detail::TimerNode& b = *timers_[rhs];
            return a.deadline < b.deadline || (a.deadline == b.deadline && a.sequence < b.sequence);
        };
        if (left < count && less(left, smallest)) {
            smallest = left;
        }
        if (right < count && less(right, smallest)) {
            smallest = right;
        }
        if (smallest == index) {
            return;
        }
        std::swap(timers_[index], timers_[smallest]);
        timers_[index]->heap_index = index;
        timers_[smallest]->heap_index = smallest;
        index = smallest;
    }
}

// ---------------------------------------------------------------------------
// File descriptors.
// ---------------------------------------------------------------------------

void Loop::add_fd(detail::FdEntry& entry) {
    fds_.push_back(&entry);  // may throw; the entry stays unlinked
    try {
        // Follow the table's geometric growth so dispatch and load shedding
        // need no allocation once a registration is committed.
        pollfds_scratch_.reserve(fds_.capacity());
        ready_entries_scratch_.reserve(fds_.capacity());
    } catch (...) {
        fds_.pop_back();
        throw;
    }
    entry.index = fds_.size() - 1;
    entry.linked = true;
    ++fds_revision_;
    CORO_TRACE("Registered fd {} for events {}", entry.fd, describe_poll_events(entry.events));
}

void Loop::remove_fd(detail::FdEntry& entry) noexcept {
    if (!entry.linked) {
        return;
    }
    entry.linked = false;
    const std::size_t index = entry.index;
    assert(index < fds_.size() && fds_[index] == &entry && "fd table/index desynchronized");
    detail::FdEntry* moved = fds_.back();  // swap-with-last: no compaction walk
    fds_[index] = moved;
    moved->index = index;
    fds_.pop_back();
    ++fds_revision_;
    CORO_TRACE("Unregistered fd {}", entry.fd);
}

}  // namespace coro

#undef CORO_TRACE
