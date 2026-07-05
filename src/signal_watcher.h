//
// Created by Kotarou on 2026/7/5.
//

#ifndef YADDNSC_SIGNAL_WATCHER_H
#define YADDNSC_SIGNAL_WATCHER_H

#include <atomic>
#include <thread>

// ---------------------------------------------------------------------------
// SignalWatcher — owns the signal-watching thread.
//
// Usage:
//   1. Call install() once at process start (before any threads).
//   2. Construct a SignalWatcher — the watching thread starts automatically.
//
// The constructor checks that install() was called beforehand and throws
// std::logic_error if not.
//
// The watching thread loops on sigwait() for SIGINT/SIGTERM with escalation:
//   - 1st SIGINT → request graceful shutdown (sets context.terminate_).
//   - 2nd SIGINT → kill(getpid(), SIGTERM) — escalate.
//   - 3rd SIGINT → kill(getpid(), SIGKILL) — hard kill.
//   - External SIGTERM → request graceful shutdown.
//
// On destruction, the watcher thread is signaled to stop and joined.
// ---------------------------------------------------------------------------
class SignalWatcher {
public:
    SignalWatcher();

    ~SignalWatcher();

    // Install the signal-watching infrastructure.
    // Blocks SIGINT/SIGTERM on the calling thread so that the watcher
    // thread can catch them via sigwait().  Must be called before any
    // threads are created, and before any SignalWatcher construction.
    static void install();

private:
    void signal_loop();

    std::thread signal_thread_;
    std::atomic<bool> shutdown_requested_{false};

    static std::atomic<bool> signals_blocked_;
    static std::atomic<bool> instance_created_;
};

#endif // YADDNSC_SIGNAL_WATCHER_H
