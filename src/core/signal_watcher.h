//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_CORE_SIGNAL_WATCHER_H
#define YADDNSC_CORE_SIGNAL_WATCHER_H

#include <atomic>
#include <stop_token>
#include <thread>

/// SignalWatcher — owns the signal-watching thread and its stop_source.
///
/// Usage:
/// @code
///   1. SignalWatcher::install();                   // once at process start
///   2. SignalWatcher watcher;                      // starts watching thread
///   3. Manager manager(config, watcher.get_stop_source());
/// @endcode
///
/// The constructor checks that install() was called beforehand and throws
/// std::logic_error if not.
///
/// The watching thread loops on sigwait() for SIGINT/SIGTERM with escalation:
///   - 1st SIGINT -> request_stop() on the internal stop_source (graceful).
///   - 2nd SIGINT -> kill(getpid(), SIGTERM) — escalate.
///   - 3rd SIGINT -> kill(getpid(), SIGKILL) — hard kill.
///   - External SIGTERM (not from escalation) -> request_stop().
///
/// @par Signal contract
/// install() blocks SIGINT, SIGTERM and SIGUSR2 on every thread for the rest
/// of the process lifetime. SIGINT/SIGTERM are consumed by the watcher thread
/// as described above. **SIGUSR2 is reserved as the destructor's internal
/// wake-up signal**: the watcher never counts or acts on it, and no other
/// component or third-party library may use it — a SIGUSR2 sent to the
/// process is silently ignored. (SIGUSR2 was chosen over SIGINT/SIGTERM so
/// the wake-up can never be mistaken for a user signal, and over SIGHUP/
/// SIGUSR1 so the conventional "reload configuration" signal and the
/// application-custom signal remain available.)
///
/// On destruction, requests stop on the watcher thread and sends a
/// process-directed SIGUSR2 to wake it out of sigwait() so that the join
/// completes promptly. The singleton guard is then released, allowing a new
/// watcher to be created afterwards.
class SignalWatcher {
public:
    SignalWatcher();

    ~SignalWatcher();

    /// Install the signal-watching infrastructure.
    ///
    /// Blocks SIGINT/SIGTERM/SIGUSR2 on the calling thread so that the
    /// watcher thread can catch them via sigwait().  Must be called before
    /// any threads are created, and before any SignalWatcher construction.
    /// See the class documentation for the full signal contract (in
    /// particular: SIGUSR2 is reserved and must not be used elsewhere).
    static void install();

    /// Return a shared handle to the internal stop state.
    /// Copying is cheap — the copy refers to the same underlying state.
    [[nodiscard]] std::stop_source get_stop_source() noexcept;

private:
    void signal_loop(std::stop_token st);

    std::stop_source stop_source_;
    std::jthread signal_thread_;

    static std::atomic<bool> signals_blocked_;
    static std::atomic<bool> instance_created_;
};

#endif  // YADDNSC_CORE_SIGNAL_WATCHER_H
