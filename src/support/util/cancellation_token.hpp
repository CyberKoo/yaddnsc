//
// Created by Kotarou on 2026/7/12.
//

#ifndef YADDNSC_UTIL_CANCELLATION_TOKEN_H
#define YADDNSC_UTIL_CANCELLATION_TOKEN_H

#include "support/util/fd.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

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
    // this same lock, and no child operation ever acquires its parent's lock.
    // This keeps trigger() allocation-free and therefore safe to keep noexcept.
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

[[nodiscard]] inline std::shared_ptr<CancellationState>
derive(const std::shared_ptr<CancellationState>& parent) {
    auto child = make_state();
    if (!parent) {
        return child;
    }

    child->parent = parent;
    bool parent_triggered = false;
    {
        std::lock_guard lock(parent->children_mutex);
        auto& children = parent->children;
        std::erase_if(children, [](const std::weak_ptr<CancellationState>& candidate) {
            return candidate.expired();
        });
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
