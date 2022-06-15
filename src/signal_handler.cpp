//
// Created by Kotarou on 2022/6/15.
//

#include "signal_handler.h"

#include <map>
#include <csignal>
#include <thread>
#include <spdlog/spdlog.h>

std::map<int, std::function<void()>> signal_handlers;

void SignalHandler::handle_signal_errno(int err, const char *func_name) {
    SPDLOG_CRITICAL("{}: {}", func_name, strerror(err));
    std::abort();
}

[[noreturn]] void *SignalHandler::handler_thread(sigset_t *sigset) {
    int signal{}, ret;
    while (true) {
        if ((ret = sigwait(sigset, &signal)) != 0) {
            handle_signal_errno(ret, "sigwait");
        }

        // call signal handler
        if (signal_handlers.find(signal) != signal_handlers.end()) {
            signal_handlers[signal]();
        } else {
            SPDLOG_CRITICAL("No signal handler registered for {}", signal);
        }
    }
}

void SignalHandler::register_handler(int signal, const std::function<void()> &handler) {
    signal_handlers.try_emplace(signal, handler);
}
