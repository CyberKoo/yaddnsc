//
// Coroutine runtime — loop implementation.
//

#include "loop.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <climits>

#include <bit>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "infrastructure/coro/cancel_scope.h"

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

/// Signals are 1-based; a 64-bit mask covers the whole POSIX range.
constexpr int SIGNAL_CAPACITY = 65;

}  // namespace

/// Signal handler: set a pending bit and nudge the loop. No allocation, no
/// locks, no second notification domain.
extern "C" void coro_signal_handler(int sig) {
    if (sig > 0 && sig < SIGNAL_CAPACITY) {
        pending_signals.fetch_or(1ULL << (sig - 1), std::memory_order_relaxed);
    }
    const int fd = signal_pipe_write_fd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        const char byte = 0;
        [[maybe_unused]] const ssize_t written = ::write(fd, &byte, 1);
    }
}

Loop::Loop() : clock_(&system_clock_), signal_waiters_(static_cast<std::size_t>(SIGNAL_CAPACITY)) {
    open_self_pipe();
}

Loop::Loop(Clock& clock) : clock_(&clock), signal_waiters_(static_cast<std::size_t>(SIGNAL_CAPACITY)) {
    open_self_pipe();
}

Loop::~Loop() noexcept {
    restore_signals();
    close_self_pipe();
}

void Loop::open_self_pipe() {
    auto [read_end, write_end] = Utils::make_pipe();
    if (!read_end || !write_end) {
        return;
    }
    pipe_read_ = std::move(read_end);
    pipe_write_ = std::move(write_end);
    fds_.push_back(FdEntry{pipe_read_.get(), POLLIN, &Loop::on_pipe_ready, this});
    signal_pipe_write_fd.store(pipe_write_.get(), std::memory_order_release);
}

void Loop::close_self_pipe() noexcept {
    int expected = pipe_write_.get();
    signal_pipe_write_fd.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
    remove_fd(pipe_read_.get());
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

void Loop::schedule(PromiseBase& frame) noexcept {
    assert(!frame.in_ready && "frame scheduled twice while already ready");
    frame.in_ready = true;
    frame.ready_next = nullptr;
    if (ready_tail_ != nullptr) {
        ready_tail_->ready_next = &frame;
    } else {
        ready_head_ = &frame;
    }
    ready_tail_ = &frame;
}

void Loop::drain_ready() {
    PromiseBase* batch = ready_head_;
    ready_head_ = nullptr;
    ready_tail_ = nullptr;
    while (batch != nullptr) {
        PromiseBase* next = batch->ready_next;
        batch->ready_next = nullptr;
        batch->in_ready = false;
        batch->self.resume();
        batch = next;
    }
}

void Loop::post(std::function<void()> fn) {
    {
        const std::lock_guard lock(inbox_mutex_);
        inbox_.push_back(std::move(fn));
    }
    wake();
}

bool Loop::process_inbox() {
    std::deque<std::function<void()>> batch;
    {
        const std::lock_guard lock(inbox_mutex_);
        batch.swap(inbox_);
    }
    if (batch.empty()) {
        return false;
    }
    for (std::function<void()>& fn : batch) {
        fn();
    }
    return true;
}

BS::thread_pool<>& Loop::offload_pool() {
    if (!pool_) {
        pool_ = std::make_unique<BS::thread_pool<>>(pool_workers_);
    }
    return *pool_;
}

void Loop::set_offload_workers(unsigned workers) noexcept {
    assert(pool_ == nullptr && "worker count must be set before the first offload");
    pool_workers_ = workers;
}

void Loop::run() {
    assert(!fds_.empty() && "loop self-pipe unavailable: nothing could ever wake the loop");
    while (!stopped_) {
        if (ready_head_ != nullptr) {
            drain_ready();
            continue;
        }
        if (process_signals()) {
            continue;
        }
        if (process_inbox()) {
            continue;
        }
        if (fire_timers()) {
            continue;
        }
        poll_once(poll_timeout_ms());
    }
}

bool Loop::process_signals() noexcept {
    const unsigned long long mask = pending_signals.exchange(0, std::memory_order_acq_rel);
    if (mask == 0) {
        return false;
    }
    bool woke = false;
    unsigned long long remaining = mask;
    while (remaining != 0) {
        const int bit = std::countr_zero(remaining);
        remaining &= remaining - 1;
        const auto index = static_cast<std::size_t>(bit + 1);
        if (index >= signal_waiters_.size()) {
            continue;
        }
        std::vector<SignalWaiter> parked;
        parked.swap(signal_waiters_[index]);
        for (SignalWaiter& entry : parked) {
            if (entry.node != nullptr) {
                if (entry.node->linked && entry.node->scope != nullptr) {
                    entry.node->scope->remove_waiter(*entry.node);
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
    TimerNode* next = heap_min();
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
    const int ready = ::poll(pollfds.data(), static_cast<nfds_t>(pollfds.size()), timeout_ms);
    if (ready <= 0) {
        return;
    }
    for (std::size_t i = 0; i < fds_.size(); ++i) {
        if (pollfds[i].revents != 0 && fds_[i].fn != nullptr) {
            fds_[i].fn(fds_[i].context, pollfds[i].revents);
        }
    }
}

bool Loop::fire_timers() noexcept {
    bool fired = false;
    while (TimerNode* next = heap_min()) {
        if (next->deadline > clock_->now()) {
            break;
        }
        TimerNode* const due = heap_pop();
        if (due->action != nullptr) {
            due->action(due->context);
        }
        fired = true;
    }
    return fired;
}

// ---------------------------------------------------------------------------
// Timer heap (indexed binary min-heap ordered by deadline then insertion).
// ---------------------------------------------------------------------------

void Loop::add_timer(TimerNode& timer, TimePoint deadline, void (*action)(void*) noexcept, void* context) {
    assert(!timer.in_heap && "timer already armed");
    timer.deadline = deadline;
    timer.sequence = timer_sequence_++;
    timer.action = action;
    timer.context = context;
    heap_push(timer);
}

void Loop::remove_timer(TimerNode& timer) noexcept {
    if (timer.in_heap) {
        heap_remove(timer);
    }
}

void Loop::heap_push(TimerNode& timer) {
    timer.heap_index = timers_.size();
    timer.in_heap = true;
    timers_.push_back(&timer);
    heap_sift_up(timer.heap_index);
}

void Loop::heap_remove(TimerNode& timer) noexcept {
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

TimerNode* Loop::heap_min() const noexcept {
    return timers_.empty() ? nullptr : timers_.front();
}

TimerNode* Loop::heap_pop() noexcept {
    TimerNode* top = timers_.front();
    heap_remove(*top);
    return top;
}

void Loop::heap_sift_up(std::size_t index) noexcept {
    while (index > 0) {
        const std::size_t parent = (index - 1) / 2;
        const TimerNode& a = *timers_[index];
        const TimerNode& b = *timers_[parent];
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
            const TimerNode& a = *timers_[lhs];
            const TimerNode& b = *timers_[rhs];
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

void Loop::add_fd(int fd, short events, void (*fn)(void*, short) noexcept, void* context) {
    fds_.push_back(FdEntry{fd, events, fn, context});
}

void Loop::remove_fd(int fd) noexcept {
    std::erase_if(fds_, [fd](const FdEntry& entry) { return entry.fd == fd; });
}

// ---------------------------------------------------------------------------
// Signals.
// ---------------------------------------------------------------------------

void Loop::arm_signal(int sig, WaitNode& node, bool* delivered) {
    if (sig <= 0 || sig >= SIGNAL_CAPACITY) {
        return;
    }
    const bool installed = std::any_of(saved_signals_.begin(), saved_signals_.end(),
                                       [sig](const auto& entry) { return entry.first == sig; });
    if (!installed) {
        struct sigaction action{};
        action.sa_handler = &coro_signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        struct sigaction previous{};
        if (::sigaction(sig, &action, &previous) == 0) {
            saved_signals_.emplace_back(sig, previous);
        }
    }
    signal_waiters_[static_cast<std::size_t>(sig)].push_back(SignalWaiter{node.waiter, &node, delivered});
}

void Loop::disarm_signal(int sig, WaitNode& node) noexcept {
    if (sig <= 0 || sig >= SIGNAL_CAPACITY) {
        return;
    }
    auto& parked = signal_waiters_[static_cast<std::size_t>(sig)];
    std::erase_if(parked, [&node](const SignalWaiter& entry) { return entry.node == &node; });
}

}  // namespace coro
