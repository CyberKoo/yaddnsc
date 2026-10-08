//
// Coroutine runtime — AsyncMutex.
//
// Persistent resources (HTTP sessions, DoH/DoT connections) are shared by
// awaiting, not by rejecting: `co_await mutex.lock()` queues a waiter in FIFO
// order and yields a move-only Guard that releases on destruction. A cancelled
// waiter leaves the queue and observes `operation_canceled`.
//

#ifndef YADDNSC_CORO_ASYNC_MUTEX_HPP
#define YADDNSC_CORO_ASYNC_MUTEX_HPP

#include <deque>
#include <system_error>
#include <utility>

#include <coroutine>

#include <expected>

#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/loop.h"

namespace coro {

/// An async, FIFO, non-rejecting mutex.
///
/// Thread safety: loop thread only. The mutex coordinates coroutines on one
/// loop; it is not a thread synchronization primitive.
/// Lifetime: a mutex must outlive every await and every Guard it hands out.
class AsyncMutex {
public:
    /// RAII ownership of the mutex; unlocks on destruction or `unlock()`.
    ///
    /// Ownership: exactly one Guard owns the lock. Move-only; a default
    /// constructed or moved-from Guard holds nothing.
    /// Failure: `unlock()` never throws.
    class Guard {
    public:
        Guard() = default;

        explicit Guard(AsyncMutex& mutex) noexcept : mutex_(&mutex) {}

        Guard(Guard&& other) noexcept : mutex_(std::exchange(other.mutex_, nullptr)) {}

        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                reset();
                mutex_ = std::exchange(other.mutex_, nullptr);
            }
            return *this;
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        ~Guard() noexcept { reset(); }

        void unlock() noexcept { reset(); }

        [[nodiscard]] explicit operator bool() const noexcept { return mutex_ != nullptr; }

    private:
        void reset() noexcept {
            if (mutex_ != nullptr) {
                AsyncMutex* mutex = std::exchange(mutex_, nullptr);
                mutex->release();
            }
        }

        AsyncMutex* mutex_ = nullptr;
    };

    /// Awaits the lock, yielding `expected<Guard, std::errc>`.
    ///
    /// Cancellation: a checkpoint — a cancelled wait yields
    /// `unexpected(operation_canceled)` and leaves the queue, without ever
    /// granting a lock that was not handed over.
    class LockAwaitable {
    public:
        explicit LockAwaitable(AsyncMutex& mutex) noexcept : mutex_(&mutex) {}

        LockAwaitable(const LockAwaitable&) = delete;
        LockAwaitable& operator=(const LockAwaitable&) = delete;

        bool await_ready() const noexcept { return false; }

        /// Acquires immediately when free, otherwise queues in FIFO order.
        /// Allocates (queue growth), so it may throw.
        template<typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle) {
            PromiseBase& promise = handle.promise();
            scope_ = promise.scope;
            if (scope_ != nullptr && scope_->cancelled()) {
                cancelled_ = true;
                return false;
            }
            if (!mutex_->locked_) {
                mutex_->locked_ = true;
                return false;  // acquired without suspending
            }
            node_.waiter = &promise;
            node_.cancelled_flag = &cancelled_;
            node_.owner = this;
            node_.on_cancel = &LockAwaitable::on_cancel;
            mutex_->queue_.push_back(&node_);
            if (scope_ != nullptr) {
                scope_->add_waiter(node_);
            }
            return true;
        }

        /// Yields the Guard, or `operation_canceled` if the wait was cancelled.
        /// Never throws.
        [[nodiscard]] std::expected<Guard, std::errc> await_resume() noexcept {
            if (node_.linked && node_.scope != nullptr) {
                node_.scope->remove_waiter(node_);
            }
            if (cancelled_) {
                return std::unexpected(std::errc::operation_canceled);
            }
            return Guard{*mutex_};
        }

    private:
        static void on_cancel(WaitNode& node) noexcept {
            auto* self = static_cast<LockAwaitable*>(node.owner);
            self->mutex_->drop_from_queue(node);
        }

        AsyncMutex* mutex_ = nullptr;
        CancelScope* scope_ = nullptr;
        WaitNode node_{};
        bool cancelled_ = false;
    };

    AsyncMutex() = default;
    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    ~AsyncMutex() = default;

    /// Returns an awaitable yielding `expected<Guard, std::errc>`.
    /// The mutex must outlive the await.
    [[nodiscard]] LockAwaitable lock() noexcept { return LockAwaitable{*this}; }

    /// True while the mutex is held by someone.
    [[nodiscard]] bool locked() const noexcept { return locked_; }

private:
    friend class LockAwaitable;

    /// Hand the lock to the next live waiter (FIFO); release it if none.
    /// Never throws: it only walks the queue and schedules.
    void release() noexcept {
        while (!queue_.empty()) {
            WaitNode* node = queue_.front();
            queue_.pop_front();
            if (node->scheduled) {
                continue;  // cancelled before the hand-off
            }
            node->scheduled = true;
            if (node->linked && node->scope != nullptr) {
                node->scope->remove_waiter(*node);
            }
            if (node->waiter != nullptr && node->waiter->loop != nullptr) {
                node->waiter->loop->schedule(*node->waiter);
            }
            return;  // lock stays held, transferred to that waiter
        }
        locked_ = false;
    }

    void drop_from_queue(WaitNode& node) noexcept {
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (*it == &node) {
                queue_.erase(it);
                return;
            }
        }
    }

    bool locked_ = false;
    std::deque<WaitNode*> queue_;
};

}  // namespace coro

#endif  // YADDNSC_CORO_ASYNC_MUTEX_HPP
