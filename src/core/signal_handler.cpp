//
// Created by Kotarou on 2026/6/29.
//

#include "signal_handler.h"

#include <unistd.h>
#include <csignal>

#include <spdlog/spdlog.h>

SignalHandler::SignalHandler() = default;

SignalHandler::~SignalHandler() {
    // If the signal thread is still blocked on sigwait(), send SIGINT
    // to wake it up so jthread can join cleanly on destruction.
    if (signal_thread_.joinable()) {
        kill(getpid(), SIGINT);
    }
}

void SignalHandler::install() {
    // Idempotent: do nothing if the handler thread already exists or
    // has already been consumed (stop_source is one-shot).
    if (signal_thread_.joinable()) {
        return;
    }

    // Signal-handler thread.
    //
    // SIGINT and SIGTERM were blocked in the main thread before Manager
    // was constructed, so all threads inherited the blocked mask.  This
    // thread is the only thread that handles them via sigwait().  Once a
    // signal arrives it requests a graceful stop — the scheduler loop
    // will break out and drain the thread pool before returning.
    signal_thread_ = std::jthread([this] {
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGINT);
        sigaddset(&sigset, SIGTERM);

        int sig;
        sigwait(&sigset, &sig);
        SPDLOG_INFO("Received exit signal, quitting...");
        if (!stop_source_.request_stop()) {
            SPDLOG_WARN("Stop request failed, current stop_possible: {}, stop_requested: {}",
                        stop_source_.stop_possible(), stop_source_.stop_requested()
            );
        }
    });
}

std::stop_source SignalHandler::get_stop_source() noexcept {
    return stop_source_;
}
