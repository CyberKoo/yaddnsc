//
// Created by Kotarou on 2026/6/29.
//

#include "signal_watcher.h"

#include <unistd.h>
#include <csignal>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "context.h"

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

    signal_thread_ = std::thread(&SignalWatcher::signal_loop, this);
}

SignalWatcher::~SignalWatcher() {
    if (signal_thread_.joinable()) {
        shutdown_requested_.store(true, std::memory_order_release);
        // Wake up the thread in case it's blocked on sigwait()
        kill(getpid(), SIGINT);
        signal_thread_.join();
    }
}

// ===== private methods ======================================================

void SignalWatcher::signal_loop() {
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);

    int sigint_count = 0;

    while (!shutdown_requested_.load(std::memory_order_acquire)) {
        int sig;
        sigwait(&sigset, &sig);

        // Re-check after waking — the destructor may have sent SIGINT to
        // unblock sigwait() after setting shutdown_requested_.  In that
        // case we exit without processing the signal, so sigint_count
        // stays unchanged and we avoid false escalation.
        if (shutdown_requested_.load(std::memory_order_acquire)) {
            break;
        }

        if (sig == SIGINT) {
            ++sigint_count;

            if (sigint_count == 1) {
                SPDLOG_INFO("Received SIGINT, initiating graceful shutdown...");
                auto &context = Context::getInstance();
                context.terminate_ = true;
                context.condition_.notify_all();
            } else if (sigint_count == 2) {
                SPDLOG_WARN("Second SIGINT received, escalating to SIGTERM...");
                kill(getpid(), SIGTERM);
            } else {
                SPDLOG_CRITICAL("Third SIGINT received, hard killing with SIGKILL...");
                kill(getpid(), SIGKILL);
            }
        } else if (sig == SIGTERM) {
            if (sigint_count == 0) {
                // External SIGTERM — not triggered by our escalation.
                SPDLOG_INFO("Received SIGTERM, shutting down...");
                auto &context = Context::getInstance();
                context.terminate_ = true;
                context.condition_.notify_all();
            }
            // If sigint_count > 0, this SIGTERM is from our escalation or
            // from a concurrent escalation race — let the loop re-check
            // shutdown_requested_ and exit cleanly.
        }
    }
}
