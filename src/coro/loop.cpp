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
#include <cstring>  // IWYU pragma: keep — clang resolves std::strerror transitively, GCC does not
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <coroutine>

#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include "coro/detail/access.h"
#include "coro/detail/frame.h"
#include "coro/detail/timer_node.h"
#include "coro/detail/wait_node.h"
#include "support/fmt.hpp"  // IWYU pragma: keep — fmt::format in the CORO_TRACE macro body

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

    /// Fire-and-forget: the job reports back through Loop::post(), so the pool
    /// never holds a result slot and never refuses work.
    void submit(std::function<void()> job) { pool.detach_task(std::move(job)); }

    [[nodiscard]] unsigned workers() const { return pool.get_thread_count(); }

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
    // Workers may still post after their awaits were cancelled. Join while
    // both the inbox and the wake pipe are alive.
    pool_.reset();
    close_self_pipe();
}

void Loop::open_self_pipe() {
    auto [read_end, write_end] = Utils::make_pipe();
    if (!read_end || !write_end) {
        throw std::runtime_error("loop self-pipe creation failed");
    }
    pipe_read_ = std::move(read_end);
    pipe_write_ = std::move(write_end);
    self_pipe_token_ = add_fd(pipe_read_.get(), POLLIN, &Loop::on_pipe_ready, this);
}

void Loop::close_self_pipe() noexcept {
    remove_fd(self_pipe_token_);
    self_pipe_token_ = 0;
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
    [[maybe_unused]] const ssize_t written = ::write(pipe_write_.get(), &byte, 1);
}

void Loop::schedule(detail::PromiseBase& frame) noexcept {
    assert(!frame.in_ready && "frame scheduled twice while already ready");
    if (frame.in_ready) {
        // Release-mode policy (rule 03): the awaitable's scheduled flag already
        // guarantees a single wakeup, so a duplicate schedule carries no
        // information; dropping it keeps the intrusive ready list intact.
        return;
    }
    frame.in_ready = true;
    frame.ready_next = nullptr;
    if (ready_tail_ != nullptr) {
        ready_tail_->ready_next = &frame;
    } else {
        ready_head_ = &frame;
    }
    ready_tail_ = &frame;
}

void Loop::defer_reap(detail::PromiseBase& frame) noexcept {
    assert(frame.continuation == nullptr && !frame.in_ready);
    frame.reap_next = reap_head_;
    reap_head_ = &frame;
}

void Loop::reap_completed() noexcept {
    while (reap_head_ != nullptr) {
        auto* frame = reap_head_;
        reap_head_ = frame->reap_next;
        frame->self.destroy();
    }
}

void Loop::drain_ready() {
    detail::PromiseBase* batch = ready_head_;
    ready_head_ = nullptr;
    ready_tail_ = nullptr;
    std::size_t resumed = 0;
    while (batch != nullptr) {
        detail::PromiseBase* next = batch->ready_next;
        batch->ready_next = nullptr;
        batch->in_ready = false;
        batch->self.resume();
        reap_completed();
        ++resumed;
        batch = next;
    }
    CORO_TRACE("Drained ready queue, resumed {} frame(s)", resumed);
}

void Loop::post(std::function<void()> fn) {
    {
        const std::lock_guard lock(inbox_mutex_);
        inbox_.push_back(std::move(fn));
    }
    // Diagnostics are emitted when the inbox is consumed on the loop thread.
    wake();
}

void Loop::process_inbox() {
    std::deque<std::function<void()>> batch;
    {
        const std::lock_guard lock(inbox_mutex_);
        batch.swap(inbox_);
    }
    if (!batch.empty()) {
        CORO_TRACE("Processing {} inbox callback(s)", batch.size());
    }
    for (std::function<void()>& fn : batch) {
        fn();
    }
}

void Loop::submit_offload(std::function<void()> job) {
    if (!pool_) {
        pool_ = std::make_unique<Pool>(pool_workers_);
        CORO_TRACE("Created offload pool with {} worker(s)", pool_workers_);
    }
    ++offload_pending_;
    try {
        pool_->submit([this, job = std::move(job)]() mutable {
            job();
            job = {};  // release user/shared captures before completion accounting
            try {
                post([this] { --offload_pending_; });
            } catch (...) {
                std::terminate();  // completion accounting cannot be lost
            }
        });
    } catch (...) {
        --offload_pending_;
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
    pool_workers_ = workers;
}

void Loop::set_trace_sink(TraceSink sink, void* context) noexcept {
    trace_sink_ = sink;
    trace_context_ = context;
}

void Loop::run() {
    assert(!fds_.empty() && "loop self-pipe unavailable: nothing could ever wake the loop");
    CORO_TRACE("run() starting");
    while (!stopped_ || offload_pending_ != 0) {
        // Worker completions and due timers run on every iteration, ahead of
        // the ready queue: a busy loop keeps re-filling the queue (every
        // resumed frame can schedule the next one), so a ready-first order
        // starves all three — a with_timeout would then fire late or never.
        process_inbox();
        fire_timers();
        if (stopped_ && offload_pending_ == 0 && ready_head_ == nullptr) {
            break;
        }
        if (ready_head_ != nullptr) {
            drain_ready();
            // The drain re-filled the queue; keep I/O moving with a
            // non-blocking scan instead of waiting for a quiet iteration.
            poll_once(0);
            continue;
        }
        poll_once(poll_timeout_ms());
    }
    CORO_TRACE("run() returning, root task completed");
}

int Loop::poll_timeout_ms() {
    if (ready_head_ != nullptr) {
        return 0;
    }
    {
        const std::lock_guard lock(inbox_mutex_);
        if (!inbox_.empty()) {
            return 0;
        }
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
    const Duration remaining = next->deadline - clock_->now();
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
    // The scratch buffers are members reused across rounds (clear() keeps the
    // allocation): a busy loop polls every iteration, so fresh locals would
    // cost a malloc/free pair per round plus growth reallocation on events.
    pollfds_scratch_.clear();
    pollfds_scratch_.reserve(fds_.size());
    for (const FdEntry& entry : fds_) {
        pollfds_scratch_.push_back(pollfd{entry.fd, entry.events, 0});
    }
    CORO_TRACE("poll() on {} fd(s), timeout {} ms", pollfds_scratch_.size(), timeout_ms);
    const int ready = ::poll(pollfds_scratch_.data(), static_cast<nfds_t>(pollfds_scratch_.size()), timeout_ms);
    if (ready < 0) {
        CORO_TRACE("poll() failed: {}", std::strerror(errno));
        return;
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
        if (pollfds_scratch_[i].revents != 0 && fds_[i].fn != nullptr) {
            ready_entries_scratch_.push_back(
                ReadyEntry{fds_[i].fn, fds_[i].context, fds_[i].fd, pollfds_scratch_[i].revents});
        }
    }
    for (const ReadyEntry& entry : ready_entries_scratch_) {
        CORO_TRACE("Fd {} ready: {}", entry.fd, describe_poll_events(entry.revents));
        entry.fn(entry.context, entry.revents);
    }
}

void Loop::fire_timers() noexcept {
    while (detail::TimerNode* next = heap_min()) {
        if (next->deadline > clock_->now()) {
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
    const auto in_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now()).count();
    CORO_TRACE("Armed timer {} (fires in {} ms)", static_cast<const void*>(&timer), in_ms);
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

detail::FdToken Loop::add_fd(int fd, short events, void (*fn)(void*, short) noexcept, void* context) {
    const detail::FdToken token = ++fd_sequence_;
    fds_.push_back(FdEntry{token, fd, events, fn, context});
    CORO_TRACE("Registered fd {} for events {} (token {})", fd, describe_poll_events(events), token);
    return token;
}

void Loop::remove_fd(detail::FdToken token) noexcept {
    const auto erased = std::erase_if(fds_, [token](const FdEntry& entry) { return entry.token == token; });
    CORO_TRACE("remove_fd(token {}) erased {} registration(s)", token, erased);
}

}  // namespace coro

#undef CORO_TRACE
