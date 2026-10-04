//
// Created by Kotarou on 2026/7/12.
//

#ifndef YADDNSC_UTIL_CANCELLATION_TOKEN_H
#define YADDNSC_UTIL_CANCELLATION_TOKEN_H

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "support/util/fd.hpp"

namespace Utils {

class CancellationSource;

namespace detail {

/// Shared state behind CancellationToken / CancellationSource.
///
/// A source owns one cancellation pipe and weak references to direct child
/// sources. Triggering a source latches and signals its entire descendant
/// tree. Each token consequently waits only on its own pipe: cancellation is
/// broadcast, never consumed by one waiter, and cannot be missed between a
/// state check and poll().
struct CancellationState {
    explicit CancellationState(std::pair<UniqueFd, UniqueFd> pipe) noexcept
        : read_end(std::move(pipe.first)), write_end(std::move(pipe.second)) {}

    UniqueFd read_end;
    UniqueFd write_end;
    std::atomic<bool> triggered{false};
    std::shared_ptr<CancellationState> parent;
    std::mutex children_mutex;
    std::vector<std::weak_ptr<CancellationState>> children;
};

[[nodiscard]] inline std::shared_ptr<CancellationState> make_state() {
    auto pipe = make_pipe();
    if (!pipe.first || !pipe.second) {
        throw std::system_error(errno, std::generic_category(), "failed to create cancellation pipe");
    }
    return std::make_shared<CancellationState>(std::move(pipe));
}

inline void trigger(const std::shared_ptr<CancellationState>& state) noexcept {
    if (!state || state->triggered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (state->write_end) {
        const char signal = 1;
        // The pipe is initially empty and each state is signalled exactly
        // once. Retry an interrupted write so waiters already in poll() are
        // reliably awakened; EAGAIN is impossible unless an external caller
        // has violated the no-drain invariant.
        ssize_t written;
        do {
            written = ::write(state->write_end.get(), &signal, sizeof(signal));
        } while (written < 0 && errno == EINTR);
    }

    // Hold the parent lock while walking: derive() can only append under
    // this same lock, and no child operation ever acquires its parent's
    // lock, so the walk is deadlock-free. noexcept is a deliberate hard
    // guarantee here, not a consequence of being allocation-free:
    // std::mutex::lock() may itself throw std::system_error, and a mutex
    // failure while latching cancellation leaves the state unrecoverable,
    // so std::terminate is the only safe outcome.
    std::lock_guard lock(state->children_mutex);
    auto& weak_children = state->children;
    for (auto it = weak_children.begin(); it != weak_children.end();) {
        if (auto child = it->lock()) {
            trigger(child);
            ++it;
        } else {
            it = weak_children.erase(it);
        }
    }
}

[[nodiscard]] inline std::shared_ptr<CancellationState> derive(const std::shared_ptr<CancellationState>& parent) {
    auto child = make_state();
    if (!parent) {
        return child;
    }

    child->parent = parent;
    bool parent_triggered = false;
    {
        std::lock_guard lock(parent->children_mutex);
        auto& children = parent->children;
        std::erase_if(children, [](const std::weak_ptr<CancellationState>& candidate) { return candidate.expired(); });
        children.push_back(child);
        parent_triggered = parent->triggered.load(std::memory_order_acquire);
    }
    if (parent_triggered) {
        trigger(child);
    }
    return child;
}

}  // namespace detail

/// A lightweight, shared-owning token for poll()-based I/O cancellation.
///
/// Cancellation is latched and broadcast: every token has a private readable
/// fd that remains readable after cancellation. It must never be drained;
/// once cancelled, all subsequent operations must fail promptly.
///
/// Hierarchies are downward-only. A token derived from another token observes
/// cancellation of the parent source, while triggering the child never affects
/// its parent or siblings.
class CancellationToken {
public:
    CancellationToken() noexcept = default;

    /// The read-end fd to include in poll(). Returns -1 for an inert token.
    [[nodiscard]] int native_handle() const noexcept { return state_ ? state_->read_end.get() : -1; }

    /// True if this token is linked to a cancellation source.
    explicit operator bool() const noexcept { return state_ != nullptr; }

    /// True after this token's source or an ancestor source was triggered.
    [[nodiscard]] bool is_triggered() const noexcept {
        return state_ && state_->triggered.load(std::memory_order_acquire);
    }

    /// Derive a child cancellation source from this token.
    [[nodiscard]] CancellationSource derive_source() const;

private:
    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state) noexcept : state_(std::move(state)) {}

    std::shared_ptr<detail::CancellationState> state_;

    friend class CancellationSource;
};

/// A source of cancellation that provides shared-owning CancellationTokens.
///
/// trigger() is thread-safe and idempotent. It latches its own state and
/// broadcasts to all current descendants; sources derived concurrently with a
/// trigger are also latched before derive() returns.
class CancellationSource {
public:
    /// Throws std::system_error if a reliable cancellation pipe cannot be
    /// created. Continuing without it could leave blocked I/O uninterruptible.
    CancellationSource() : state_(detail::make_state()) {}

    [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(state_); }

    [[nodiscard]] CancellationSource derive() const { return CancellationSource(detail::derive(state_)); }

    void trigger() const noexcept { detail::trigger(state_); }

    [[nodiscard]] bool is_triggered() const noexcept {
        return state_ && state_->triggered.load(std::memory_order_acquire);
    }

    /// Owns one stop_token → source registration.
    ///
    /// Neither copyable nor movable: a stop registration is a unique resource
    /// that cannot be duplicated or handed on, and std::stop_callback is itself
    /// non-copyable and non-movable. A binding is therefore constructed exactly
    /// once, in place — typically as a direct member whose initialiser is the
    /// prvalue returned by bind().
    ///
    /// Destroying the binding unregisters it; later stop requests no longer
    /// reach the source. The callback captures the shared cancellation state
    /// rather than the source object, so it never dereferences a source that
    /// has been destroyed — a binding created in the caller's scope is safe
    /// even though the source it was built from is a by-reference dependency.
    class StopBinding {
    public:
        StopBinding(const StopBinding&) = delete;
        StopBinding& operator=(const StopBinding&) = delete;
        StopBinding(StopBinding&&) = delete;
        StopBinding& operator=(StopBinding&&) = delete;
        ~StopBinding() = default;

    private:
        friend class CancellationSource;

        // Takes the registration's parts rather than a stop_callback: the
        // member is direct-initialised from them, which avoids moving the
        // (non-movable) callback in or out.
        StopBinding(std::stop_token stop, const std::shared_ptr<detail::CancellationState>& state) noexcept
            : callback_(std::move(stop), [state] { detail::trigger(state); }) {}

        std::stop_callback<std::function<void()>> callback_;
    };

    /// Project a std::stop_token onto this source: requesting @p stop
    /// triggers this source and, through it, every token derived from it.
    ///
    /// The two cancellation domains stay separate by design. std::stop_token
    /// drives cooperative control flow and has no file descriptor, so it can
    /// neither join a poll() set nor express a child source. bind() is the one
    /// place where stop-driven control flow becomes poll-based I/O abort.
    ///
    /// A stop already requested when bind() is called triggers the source
    /// inline, so no cancellation window opens between the binding and the
    /// first I/O wait.
    [[nodiscard]] StopBinding bind(std::stop_token stop) const { return StopBinding(std::move(stop), state_); }

private:
    explicit CancellationSource(std::shared_ptr<detail::CancellationState> state) noexcept : state_(std::move(state)) {}

    std::shared_ptr<detail::CancellationState> state_;

    friend class CancellationToken;
};

inline CancellationSource CancellationToken::derive_source() const {
    return CancellationSource(detail::derive(state_));
}

}  // namespace Utils

#endif  // YADDNSC_UTIL_CANCELLATION_TOKEN_H
