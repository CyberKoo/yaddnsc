//
// Created by Kotarou on 2026/6/29.
//

#include "signal_watcher.h"

#include <csignal>
#include <stdexcept>
#include <string_view>

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
    if (pthread_sigmask(SIG_BLOCK, &sigset, nullptr) != 0) {
        SPDLOG_CRITICAL("Failed to block SIGINT/SIGTERM, errno: {}", errno);
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
    // If the signal thread is still blocked on sigwait(), mark the
    // jthread's stop_token and then send SIGINT to wake it up so
    // that ~jthread() can join cleanly.
    if (signal_thread_.joinable()) {
        signal_thread_.request_stop();
        kill(getpid(), SIGINT);
    }
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

        // If stop was requested (e.g. from the destructor's wake-up SIGINT),
        // exit immediately without counting the wake-up signal.
        if (st.stop_requested()) {
            break;
        }

        if (sig == SIGINT) {
            ++sigint_count;

            if (sigint_count == 1) {
                request_stop("Received SIGINT, initiating graceful shutdown...");
            } else if (sigint_count == 2) {
                SPDLOG_WARN("Second SIGINT received, escalating to SIGTERM...");
                kill(getpid(), SIGTERM);
            } else {
                SPDLOG_CRITICAL("Third SIGINT received, hard killing with SIGKILL...");
                kill(getpid(), SIGKILL);
            }
        } else if (sig == SIGTERM) {
            if (sigint_count == 0) {
                // External SIGTERM — not triggered by our own escalation.
                request_stop("Received SIGTERM, shutting down...");
            } else {
                // SIGTERM when sigint_count > 0: from our escalation or from
                // the destructor's wake-up — skip, the while-loop check above
                // will see st.stop_requested() and exit.
                SPDLOG_TRACE("SIGTERM suppressed during SIGINT escalation (sigint_count={})", sigint_count);
            }
        }
    }
}
