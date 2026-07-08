//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_CORE_SIGNAL_WATCHER_H
#define YADDNSC_CORE_SIGNAL_WATCHER_H

#include <atomic>
#include <stop_token>
#include <thread>

#include "mixin.h"

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
/// On destruction, if the watcher thread is still blocked on sigwait(), it
/// requests stop on the jthread's own stop_token and then sends SIGINT to
/// wake it up so that ~jthread() can join cleanly.
class SignalWatcher
{
public:
  SignalWatcher();

  ~SignalWatcher();

  /// Install the signal-watching infrastructure.
  ///
  /// Blocks SIGINT/SIGTERM on the calling thread so that the watcher
  /// thread can catch them via sigwait().  Must be called before any
  /// threads are created, and before any SignalWatcher construction.
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

  [[maybe_unused, no_unique_address]] NoCopy _nc_;
  [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif  // YADDNSC_CORE_SIGNAL_WATCHER_H
