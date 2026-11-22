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

namespace {

void request_shutdown() {
    auto &context = Context::getInstance();
    {
        std::lock_guard lock(context.mutex_);
        context.terminate_.store(true, std::memory_order_release);
    }
    context.condition_.notify_all();
}

}  // namespace

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
        int sig = 0;
        const int error = sigwait(&sigset, &sig);
        if (error != 0) {
            SPDLOG_ERROR("sigwait failed with error {}", error);
            request_shutdown();
            break;
        }

        if (shutdown_requested_.load(std::memory_order_acquire)) {
            break;
        }

        if (sig == SIGINT) {
            ++sigint_count;

            if (sigint_count == 1) {
                SPDLOG_INFO("Received SIGINT, initiating graceful shutdown...");
                request_shutdown();
            } else {
                SPDLOG_CRITICAL("Second SIGINT received, forcing immediate termination");
                ::_exit(128 + SIGINT);
            }
        } else if (sig == SIGTERM) {
            SPDLOG_INFO("Received SIGTERM, shutting down...");
            request_shutdown();
        }
    }
}
