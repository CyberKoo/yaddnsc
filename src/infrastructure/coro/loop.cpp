//
// Coroutine runtime — loop implementation.
//

#include "loop.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <bit>
#include <fcntl.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include "infrastructure/coro/detail/access.h"
#include "infrastructure/coro/detail/timer_node.h"

#include "BS_thread_pool.hpp"

namespace coro {

namespace {

// Process-wide state shared with the signal handler.
//
// Justification for globals (rule 04: "prefer explicit ownership over global
// mutable state"; rule 02: "global/singleton ports must not be introduced"):
// a POSIX signal handler is an extern "C" free function — it cannot capture a
// Loop pointer, and async-signal-safety forbids taking a lock, allocating or
// touching anything but lock-free atomics and write(2). These are plain data
// (not a port), they are set/cleared by the owning Loop's lifetime, and a
// process runs one loop at a time, which is the documented model.
//
// Synchronization strategy: single-word atomics only. relaxed is enough for
// the handler (it publishes a bit and a byte; every ordering that matters is
// established by the self-pipe read), and the loop uses acquire on its reads
// so it observes the handler's bit before it acts on it.

/// Write end of the process self-pipe; -1 when no loop owns a pipe.
std::atomic<int> signal_pipe_write_fd{-1};
/// One pending bit per signal number (bit 0 == signal 1).
std::atomic<unsigned long long> pending_signals{0};
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<unsigned long long>::is_always_lock_free);

/// Signals are 1-based; a 64-bit mask covers the whole POSIX range.
constexpr int SIGNAL_CAPACITY = 65;

/// Offload pool sizing policy: min(hardware cores, OFFLOAD_WORKER_LIMIT), at
/// least OFFLOAD_MIN_WORKERS.
constexpr unsigned OFFLOAD_MIN_WORKERS = 2;
constexpr unsigned OFFLOAD_WORKER_LIMIT = 4;

/// Render poll() event bits as a readable "POLLIN|POLLHUP" string for traces.
[[maybe_unused]] std::string describe_poll_events(short events) {
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

/// Signal handler: set a pending bit and nudge the loop. No allocation, no
/// locks, no second notification domain.
extern "C" void coro_signal_handler(int sig) {
    const int saved_errno = errno;
    if (sig > 0 && sig < SIGNAL_CAPACITY) {
        pending_signals.fetch_or(1ULL << (sig - 1), std::memory_order_relaxed);
    }
    const int fd = signal_pipe_write_fd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        const char byte = 0;
        [[maybe_unused]] const ssize_t written = ::write(fd, &byte, 1);
    }
    errno = saved_errno;
}

Loop::Loop() : clock_(&system_clock_), signal_waiters_(static_cast<std::size_t>(SIGNAL_CAPACITY)) {
    open_self_pipe();
}

Loop::Loop(Clock& clock) : clock_(&clock), signal_waiters_(static_cast<std::size_t>(SIGNAL_CAPACITY)) {
    open_self_pipe();
}

Loop::~Loop() noexcept {
    // Workers may still post after their awaits were cancelled. Join while
    // both the inbox and the wake pipe are alive.
    pool_.reset();
    restore_signals();
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
    signal_pipe_write_fd.store(pipe_write_.get(), std::memory_order_release);
}

void Loop::close_self_pipe() noexcept {
    int expected = pipe_write_.get();
    signal_pipe_write_fd.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
    remove_fd(self_pipe_token_);
    self_pipe_token_ = 0;
    pipe_read_.reset();
    pipe_write_.reset();
}

void Loop::restore_signals() noexcept {
    for (const auto& [sig, action] : saved_signals_) {
        ::sigaction(sig, &action, nullptr);
    }
    saved_signals_.clear();
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
    frame.in_ready = true;
    frame.ready_next = nullptr;
    if (ready_tail_ != nullptr) {
        ready_tail_->ready_next = &frame;
    } else {
        ready_head_ = &frame;
    }
    ready_tail_ = &frame;
    SPDLOG_TRACE("scheduled frame {}", static_cast<const void*>(&frame));
}

void Loop::drain_ready() {
    detail::PromiseBase* batch = ready_head_;
    ready_head_ = nullptr;
    ready_tail_ = nullptr;
    [[maybe_unused]] std::size_t resumed = 0;
    while (batch != nullptr) {
        detail::PromiseBase* next = batch->ready_next;
        batch->ready_next = nullptr;
        batch->in_ready = false;
        batch->self.resume();
        ++resumed;
        batch = next;
    }
    SPDLOG_TRACE("drained ready queue, resumed {} frame(s)", resumed);
}

void Loop::post(std::function<void()> fn) {
    {
        const std::lock_guard lock(inbox_mutex_);
        inbox_.push_back(std::move(fn));
    }
    SPDLOG_TRACE("posted a cross-thread callback to the inbox");
    wake();
}

void Loop::process_inbox() {
    std::deque<std::function<void()>> batch;
    {
        const std::lock_guard lock(inbox_mutex_);
        batch.swap(inbox_);
    }
    if (!batch.empty()) {
        SPDLOG_TRACE("processing {} inbox callback(s)", batch.size());
    }
    for (std::function<void()>& fn : batch) {
        fn();
    }
}

// The offload worker pool. Defined here so BS_thread_pool.hpp reaches exactly
// one translation unit: loop.h declares Pool and nothing else names BS.
class Loop::Pool {
public:
    explicit Pool(unsigned workers) : pool(workers) {}

    /// Fire-and-forget: the job reports back through Loop::post(), so the pool
    /// never holds a result slot and never refuses work.
    void submit(std::function<void()> job) { pool.detach_task(std::move(job)); }

    [[nodiscard]] unsigned workers() const { return pool.get_thread_count(); }

private:
    BS::thread_pool<> pool;
};

void Loop::submit_offload(std::function<void()> job) {
    if (!pool_) {
        pool_ = std::make_unique<Pool>(pool_workers_);
    }
    pool_->submit(std::move(job));
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

void Loop::run() {
    assert(!fds_.empty() && "loop self-pipe unavailable: nothing could ever wake the loop");
    SPDLOG_TRACE("run() starting");
    while (!stopped_) {
        // Signals are checked before everything else: a busy loop (e.g. the
        // cancellation storm of a shutdown drain) must not starve them, and a
        // second SIGINT must stay deliverable while the drain runs.
        if (process_signals()) {
            continue;
        }
        // Worker completions and due timers run on every iteration, ahead of
        // the ready queue: a busy loop keeps re-filling the queue (every
        // resumed frame can schedule the next one), so a ready-first order
        // starves all three — a with_timeout would then fire late or never.
        process_inbox();
        fire_timers();
        if (ready_head_ != nullptr) {
            drain_ready();
            // The drain re-filled the queue; keep I/O moving with a
            // non-blocking scan instead of waiting for a quiet iteration.
            poll_once(0);
            continue;
        }
        poll_once(poll_timeout_ms());
    }
    SPDLOG_TRACE("run() returning, root task completed");
}

bool Loop::process_signals() noexcept {
    const unsigned long long mask = pending_signals.exchange(0, std::memory_order_acq_rel);
    if (mask == 0) {
        return false;
    }
    SPDLOG_TRACE("processing pending signal mask {:#x}", mask);
    bool woke = false;
    unsigned long long remaining = mask;
    while (remaining != 0) {
        const int bit = std::countr_zero(remaining);
        remaining &= remaining - 1;
        const auto index = static_cast<std::size_t>(bit) + 1;
        if (index >= signal_waiters_.size()) {
            continue;
        }
        std::vector<SignalWaiter> parked;
        parked.swap(signal_waiters_[index]);
        if (!parked.empty()) {
            SPDLOG_TRACE("signal {} wakes {} waiter(s)", bit + 1, parked.size());
        }
        for (SignalWaiter& entry : parked) {
            if (entry.node != nullptr) {
                if (entry.node->linked && entry.node->scope != nullptr) {
                    detail::ScopeAccess::remove_waiter(*entry.node->scope, *entry.node);
                }
                entry.node->scheduled = true;
            }
            if (entry.delivered != nullptr) {
                *entry.delivered = true;
            }
            if (entry.waiter != nullptr) {
                schedule(*entry.waiter);
                woke = true;
            }
        }
    }
    return woke;
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
    if (pending_signals.load(std::memory_order_acquire) != 0) {
        return 0;
    }
    detail::TimerNode* next = heap_min();
    if (next == nullptr) {
        // A manual clock cannot be advanced by a blocking poll; return
        // immediately so the loop stays responsive to post()/signals.
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
    std::vector<pollfd> pollfds;
    pollfds.reserve(fds_.size());
    for (const FdEntry& entry : fds_) {
        pollfds.push_back(pollfd{entry.fd, entry.events, 0});
    }
    SPDLOG_TRACE("poll() on {} fd(s), timeout {} ms", pollfds.size(), timeout_ms);
    const int ready = ::poll(pollfds.data(), static_cast<nfds_t>(pollfds.size()), timeout_ms);
    if (ready < 0) {
        SPDLOG_TRACE("poll() failed: {}", std::strerror(errno));
        return;
    }
    if (ready == 0) {
        return;
    }
    SPDLOG_TRACE("poll() reported {} ready fd(s)", ready);

    // Snapshot the ready entries before dispatching: a callback may remove its
    // own registration (or another one), which would invalidate an index walk
    // over fds_ while it is being iterated.
    struct ReadyEntry {
        void (*fn)(void*, short) noexcept;
        void* context;
        int fd;
        short revents;
    };

    std::vector<ReadyEntry> ready_entries;
    for (std::size_t i = 0; i < fds_.size(); ++i) {
        if (pollfds[i].revents != 0 && fds_[i].fn != nullptr) {
            ready_entries.push_back(ReadyEntry{fds_[i].fn, fds_[i].context, fds_[i].fd, pollfds[i].revents});
        }
    }
    for (const ReadyEntry& entry : ready_entries) {
        SPDLOG_TRACE("fd {} ready: {}", entry.fd, describe_poll_events(entry.revents));
        entry.fn(entry.context, entry.revents);
    }
}

void Loop::fire_timers() noexcept {
    while (detail::TimerNode* next = heap_min()) {
        if (next->deadline > clock_->now()) {
            break;
        }
        detail::TimerNode* const due = heap_pop();
        SPDLOG_TRACE("firing timer {}", static_cast<const void*>(due));
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
    [[maybe_unused]] const auto in_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now()).count();
    SPDLOG_TRACE("armed timer {} (fires in {} ms)", static_cast<const void*>(&timer), in_ms);
}

void Loop::remove_timer(detail::TimerNode& timer) noexcept {
    if (timer.in_heap) {
        SPDLOG_TRACE("disarmed timer {}", static_cast<const void*>(&timer));
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
    SPDLOG_TRACE("registered fd {} for events {} (token {})", fd, describe_poll_events(events), token);
    return token;
}

void Loop::remove_fd(detail::FdToken token) noexcept {
    [[maybe_unused]] const auto erased =
        std::erase_if(fds_, [token](const FdEntry& entry) { return entry.token == token; });
    SPDLOG_TRACE("remove_fd(token {}) erased {} registration(s)", token, erased);
}

// ---------------------------------------------------------------------------
// Signals.
// ---------------------------------------------------------------------------

void Loop::arm_signal(int sig, detail::WaitNode& node, bool* delivered) {
    if (sig <= 0 || sig >= SIGNAL_CAPACITY || sig == SIGKILL || sig == SIGSTOP) {
        throw std::invalid_argument("signal cannot be awaited");
    }
    auto& waiters = signal_waiters_[static_cast<std::size_t>(sig)];
    waiters.reserve(waiters.size() + 1);
    const bool installed = std::any_of(saved_signals_.begin(), saved_signals_.end(),
                                       [sig](const auto& entry) { return entry.first == sig; });
    if (!installed) {
        saved_signals_.reserve(saved_signals_.size() + 1);
        struct sigaction action{};
        action.sa_handler = &coro_signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        struct sigaction previous{};
        if (::sigaction(sig, &action, &previous) != 0) {
            throw std::system_error(errno, std::generic_category(), "sigaction");
        }
        saved_signals_.emplace_back(sig, previous);
    }
    waiters.push_back(SignalWaiter{node.waiter, &node, delivered});
    SPDLOG_TRACE("armed signal waiter for signal {}", sig);
}

void Loop::disarm_signal(int sig, detail::WaitNode& node) noexcept {
    if (sig <= 0 || sig >= SIGNAL_CAPACITY) {
        return;
    }
    auto& parked = signal_waiters_[static_cast<std::size_t>(sig)];
    [[maybe_unused]] const auto erased =
        std::erase_if(parked, [&node](const SignalWaiter& entry) { return entry.node == &node; });
    SPDLOG_TRACE("disarmed signal waiter for signal {} ({} waiter(s) removed)", sig, erased);
}

}  // namespace coro
