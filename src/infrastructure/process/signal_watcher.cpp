//
// Created by Kotarou on 2026/6/29.
//

#include "signal_watcher.h"

#include <csignal>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string_view>

#include <errno.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

// ===== static data members ==================================================

std::atomic<bool> SignalWatcher::signals_blocked_{false};
std::atomic<bool> SignalWatcher::instance_created_{false};

// ===== static methods =======================================================

void SignalWatcher::install() {
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    sigaddset(&sigset, SIGUSR2);  // reserved as the destructor's wake-up signal
    if (pthread_sigmask(SIG_BLOCK, &sigset, nullptr) != 0) {
        SPDLOG_CRITICAL("Failed to block SIGINT/SIGTERM/SIGUSR2, errno: {}", errno);
        std::terminate();
    }
    signals_blocked_.store(true, std::memory_order_release);
}

// ===== public methods =======================================================

SignalWatcher::SignalWatcher() {
    if (!signals_blocked_.load(std::memory_order_acquire)) {
        throw std::logic_error("SignalWatcher: install() must be called before construction");
    }

    bool expected = false;
    if (!instance_created_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        throw std::logic_error("SignalWatcher: only one instance allowed");
    }

    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    signal_thread_ = std::jthread([this](std::stop_token st) { signal_loop(st); });
}

SignalWatcher::~SignalWatcher() {
    if (signal_thread_.joinable()) {
        signal_thread_.request_stop();
        // Wake the watcher thread out of sigwait() so that the join below
        // completes promptly. The signal is process-directed: SIGUSR2 is
        // blocked in every thread (install()) and appears only in the watcher
        // thread's sigwait set, so it is either consumed there or stays
        // pending until the next sigwait() — there is no path where the
        // thread blocks forever. A stale pending SIGUSR2 is ignored by
        // signal_loop(), so a later watcher is unaffected.
        kill(getpid(), SIGUSR2);
        signal_thread_.join();
    }
    // Release the singleton guard so a new watcher can be created (e.g. in
    // tests); the join above guarantees no watcher thread is still alive.
    instance_created_.store(false, std::memory_order_release);
}

std::stop_source SignalWatcher::get_stop_source() noexcept {
    return stop_source_;
}

// ===== private methods ======================================================

// NOLINTNEXTLINE(performance-unnecessary-value-param) — std::stop_token must be by-value for jthread
void SignalWatcher::signal_loop(std::stop_token st) {
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    sigaddset(&sigset, SIGUSR2);  // destructor wake-up — ignored below

    auto request_stop = [this](std::string_view reason) {
        SPDLOG_INFO("{}", reason);
        if (!stop_source_.request_stop()) {
            SPDLOG_WARN("Stop request failed, stop_possible: {}, stop_requested: {}", stop_source_.stop_possible(),
                        stop_source_.stop_requested());
        }
    };

    int sigint_count = 0;

    while (!st.stop_requested()) {
        int sig;
        sigwait(&sigset, &sig);

        // Shutdown wins over any pending signal — exit immediately once stop
        // has been requested (e.g. by the destructor's wake-up SIGUSR2).
        if (st.stop_requested()) {
            break;
        }

        if (sig == SIGUSR2) {
            // Destructor wake-up (or a stale one from a previous watcher):
            // never counted as a user signal, never acted upon.
            continue;
        }

        if (sig == SIGINT) {
            ++sigint_count;

            if (sigint_count == 1) {
                request_stop("Received SIGINT, initiating graceful shutdown (press Ctrl-C again to force quit)...");
            } else {
                // Escalating through SIGTERM is a no-op: SIGTERM is blocked
                // process-wide and would be consumed by this very thread.
                // Terminate directly with the conventional 128+SIGINT status.
                SPDLOG_CRITICAL("Second SIGINT received, forcing immediate termination");
                ::_exit(128 + SIGINT);
            }
        } else if (sig == SIGTERM) {
            // SIGTERM is never self-sent (escalation terminates directly), so
            // every SIGTERM observed here is external.
            request_stop("Received SIGTERM, shutting down...");
        }
    }
}
