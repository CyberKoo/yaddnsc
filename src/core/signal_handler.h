//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_CORE_SIGNAL_HANDLER_H
#define YADDNSC_CORE_SIGNAL_HANDLER_H

#include <thread>
#include <stop_token>

#include "mixin.h"

// ---------------------------------------------------------------------------
// SignalHandler — owns the signal-waiting thread and its stop_source.
//
// install() spawns a jthread that blocks on sigwait() for SIGINT/SIGTERM.
// When a signal arrives it calls request_stop() on the internal stop_source.
// The caller can retrieve the stop_source via get_stop_source() and pass it
// to the Scheduler so that the event loop exits gracefully.
//
// In the destructor, if the signal thread is still blocked on sigwait()
// (e.g. run() was never called, or an exception escaped the scheduler), the
// destructor sends SIGTERM to wake it up so that ~jthread() can join cleanly.
// SIGTERM is blocked in the process's signal mask (set up in main() before
// Manager was constructed), so the kill() is harmless.
//
// Extracted from Manager::Impl to keep signal-lifecycle management separate
// from the scheduler and orchestrator.
// ---------------------------------------------------------------------------
class SignalHandler {
public:
    SignalHandler();

    ~SignalHandler();

    // Install the signal-handling thread.  Idempotent.
    void install();

    // Retrieve the stop_source that will be triggered on SIGINT/SIGTERM.
    std::stop_source get_stop_source() noexcept;

private:
    std::stop_source stop_source_;
    std::jthread signal_thread_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif // YADDNSC_CORE_SIGNAL_HANDLER_H
