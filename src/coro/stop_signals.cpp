#include "stop_signals.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <coroutine>

#include <sys/types.h>
#include <unistd.h>

#include "coro/cancel_scope.h"
#include "coro/fd_wait.hpp"
#include "coro/scope.hpp"
#include "coro/sleep.hpp"

namespace {

// A C signal handler cannot capture its owner. These lock-free values belong
// to the single StopSignals instance; no application dependency is global.
// rules/04-quality-and-process.md prefers explicit ownership over global mutable
// state; this is process signal state, not a global/singleton port prohibited
// by rules/02-implementation.md (Function & Constructor Signatures).
std::atomic<int> signal_write_fd{-1};
std::atomic<int> sigint_count{0};
std::atomic<int> stop_requested{0};
static_assert(std::atomic<int>::is_always_lock_free);

extern "C" void handle_stop_signal(int sig) {
    const int saved_errno = errno;
    if (sig == SIGINT && sigint_count.fetch_add(1, std::memory_order_relaxed) >= 1) {
        ::_exit(128 + SIGINT);
    }
    if (stop_requested.exchange(1, std::memory_order_relaxed) == 0) {
        const int fd = signal_write_fd.load(std::memory_order_relaxed);
        if (fd >= 0) {
            const char byte = 1;
            ssize_t written;
            do {
                written = ::write(fd, &byte, 1);
            } while (written < 0 && errno == EINTR);
        }
    }
    errno = saved_errno;
}

}  // namespace

coro::StopSignals::StopSignals() {
    auto [read_end, write_end] = Utils::make_pipe();
    if (!read_end || !write_end) {
        throw std::system_error(errno, std::generic_category(), "stop signal pipe");
    }
    int unused = -1;
    if (!signal_write_fd.compare_exchange_strong(unused, write_end.get())) {
        throw std::logic_error("Only one stop signal owner may exist");
    }
    stop_requested.store(0, std::memory_order_relaxed);
    sigint_count.store(0, std::memory_order_relaxed);

    struct sigaction action{};
    action.sa_handler = &handle_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    action.sa_flags = SA_RESTART;
    if (::sigaction(SIGINT, &action, &previous_int_) != 0) {
        const int error = errno;
        signal_write_fd.store(-1);
        throw std::system_error(error, std::generic_category(), "sigaction(SIGINT)");
    }
    if (::sigaction(SIGTERM, &action, &previous_term_) != 0) {
        const int error = errno;
        ::sigaction(SIGINT, &previous_int_, nullptr);
        signal_write_fd.store(-1);
        throw std::system_error(error, std::generic_category(), "sigaction(SIGTERM)");
    }
    read_ = std::move(read_end);
    write_ = std::move(write_end);
}

coro::StopSignals::~StopSignals() noexcept {
    ::sigaction(SIGINT, &previous_int_, nullptr);
    ::sigaction(SIGTERM, &previous_term_, nullptr);
    signal_write_fd.store(-1);
}

// Keep observation tied to the owner of the installed process handlers.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool coro::StopSignals::requested() const noexcept {
    return stop_requested.load(std::memory_order_relaxed) != 0;
}

coro::Task<void> coro::StopSignals::wait() const {
    (co_await coro::current_scope()).throw_if_cancelled();
    if (!requested()) {
        // The byte stays in the pipe: readiness is a durable stop latch, so a
        // signal between the check and registration cannot be lost.
        if (const std::error_code error [[maybe_unused]] = co_await coro::wait_readable(read_.get())) {
            // The event source is impaired (a loop-level poll failure), so fd
            // readiness is unreachable for now. The signal handler's atomic
            // latch still works; observe it on the timer heap — timers do not
            // depend on poll() — until a stop lands or the wait is cancelled.
            while (!requested()) {
                co_await coro::sleep_for(std::chrono::milliseconds{20});
            }
        }
    }
}
