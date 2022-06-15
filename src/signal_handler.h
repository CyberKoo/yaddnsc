//
// Created by Kotarou on 2022/6/15.
//

#ifndef YADDNSC_SIGNAL_HANDLER_H
#define YADDNSC_SIGNAL_HANDLER_H

#include <csignal>
#include <functional>

namespace SignalHandler {
    [[noreturn]] void *handler_thread(sigset_t *);

    void handle_signal_errno(int, const char *);

    void register_handler(int, const std::function<void()> &);

    template<std::size_t N>
    sigset_t block_signal(const int (&signals)[N]) {
        sigset_t sigset;
        sigemptyset(&sigset);
        for (auto signal: signals) {
            sigaddset(&sigset, signal);
        }

        auto ret = pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
        if (ret != 0) {
            handle_signal_errno(ret, "pthread_sigmask");
        }

        return sigset;
    }
}

#endif //YADDNSC_SIGNAL_HANDLER_H
