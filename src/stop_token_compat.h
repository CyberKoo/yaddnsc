//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_STOP_TOKEN_COMPAT_H
#define YADDNSC_STOP_TOKEN_COMPAT_H

// Drop-in compatibility header for std::stop_token, std::stop_source,
// std::stop_callback, and std::jthread.  Uses the native standard-library
// implementations when available (Apple Clang >= 15 / Xcode >= 15,
// GCC >= 11, Clang >= 14, MSVC >= 2022 17.4) and falls back to a minimal
// polyfill otherwise.

#include <version>

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L

#include <stop_token>
#include <thread>

#else  // no native support – provide a minimal polyfill

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <list>
#include <thread>
#include <type_traits>
#include <utility>

// -----------------------------------------------------------------------
// Minimal polyfill for std::stop_token / std::stop_source /
// std::stop_callback / std::jthread
// -----------------------------------------------------------------------
// These are simplified implementations that cover only the subset of the
// API actually used by yaddnsc.  They are *not* intended to be full
// conforming replacements.

namespace yaddnsc_detail {

// Shared state used by stop_token, stop_source, and stop_callback.
class stop_state {
public:
    stop_state() = default;
    stop_state(const stop_state &) = delete;
    stop_state &operator=(const stop_state &) = delete;

    [[nodiscard]] bool stop_requested() const noexcept {
        return stopped_.load(std::memory_order_acquire);
    }

    bool request_stop() noexcept {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
            return false; // already stopped
        }
        // Snapshot callbacks under lock, then invoke them outside the lock
        // to avoid any lock-order inversion with user code.
        callback_list cbs;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cbs.swap(callbacks_);
        }
        for (auto &cb : cbs) {
            if (cb) cb();
        }
        return true;
    }

    using callback_list = std::list<std::function<void()>>;
    using callback_handle = callback_list::iterator;

    callback_handle add_callback(std::function<void()> fn) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!stopped_.load(std::memory_order_relaxed)) {
            callbacks_.push_back(std::move(fn));
            return std::prev(callbacks_.end());
        }
        lock.unlock();
        fn(); // stop already requested – invoke immediately
        return callbacks_.end(); // sentinel: not registered
    }

    void remove_callback(callback_handle it) noexcept {
        if (it != callbacks_.end()) {
            std::unique_lock<std::mutex> lock(mutex_);
            callbacks_.erase(it);
        }
    }

private:
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    callback_list callbacks_;
};

class stop_token {
public:
    stop_token() = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ && state_->stop_requested();
    }

    [[nodiscard]] bool stop_possible() const noexcept {
        return state_ != nullptr;
    }

private:
    friend class stop_source;
    template<typename> friend class stop_callback;

    explicit stop_token(std::shared_ptr<stop_state> state)
        : state_(std::move(state)) {}

    std::shared_ptr<stop_state> state_;
};

class stop_source {
public:
    stop_source()
        : state_(std::make_shared<stop_state>()) {}

    stop_source(const stop_source &) = default;
    stop_source(stop_source &&) noexcept = default;
    stop_source &operator=(const stop_source &) = default;
    stop_source &operator=(stop_source &&) noexcept = default;

    [[nodiscard]] stop_token get_token() const noexcept {
        return stop_token(state_);
    }

    [[nodiscard]] bool stop_possible() const noexcept {
        return true;
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ && state_->stop_requested();
    }

    bool request_stop() noexcept {
        return state_ && state_->request_stop();
    }

private:
    std::shared_ptr<stop_state> state_;
};

template<typename Callback>
class stop_callback {
public:
    using callback_type = Callback;

    stop_callback(const stop_token &st, Callback cb)
        : state_(st.state_), cb_(std::move(cb)) {
        if (state_) {
            handle_ = state_->add_callback(std::function<void()>(cb_));
        }
    }

    ~stop_callback() {
        if (state_) {
            state_->remove_callback(handle_);
        }
    }

    stop_callback(const stop_callback &) = delete;
    stop_callback &operator=(const stop_callback &) = delete;

private:
    std::shared_ptr<stop_state> state_;
    Callback cb_;
    stop_state::callback_handle handle_{};
};

// -----------------------------------------------------------------------
// Minimal jthread: wraps std::thread with auto-join on destruction.
//
// The standard jthread additionally owns an internal stop_source and will
// automatically pass its stop_token to the callable when the callable
// accepts one.  yaddnsc always passes an explicit stop_token from the
// global stop_source, so we omit that machinery for simplicity.
// -----------------------------------------------------------------------
class jthread {
public:
    jthread() noexcept = default;

    template<typename Fn, typename... Args,
             typename = std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, jthread>>>
    explicit jthread(Fn &&fn, Args &&...args)
        : thread_(std::forward<Fn>(fn), std::forward<Args>(args)...) {}

    jthread(const jthread &) = delete;
    jthread(jthread &&other) noexcept
        : thread_(std::move(other.thread_)) {}

    jthread &operator=(const jthread &) = delete;
    jthread &operator=(jthread &&other) noexcept {
        if (this != &other) {
            if (thread_.joinable()) {
                thread_.join();
            }
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    ~jthread() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::thread::id get_id() const noexcept {
        return thread_.get_id();
    }

    [[nodiscard]] bool joinable() const noexcept {
        return thread_.joinable();
    }

    void join() { thread_.join(); }
    void detach() { thread_.detach(); }

    std::thread &as_thread() noexcept { return thread_; }
    [[nodiscard]] const std::thread &as_thread() const noexcept { return thread_; }

private:
    std::thread thread_;
};

} // namespace yaddnsc_detail

// Inject into namespace std so that the rest of the code can use the
// standard names without any source-level changes.  This is well-defined
// because we only reach here when the standard library does *not* provide
// these types, so no ODR conflict can arise.
namespace std {
    using yaddnsc_detail::stop_token;
    using yaddnsc_detail::stop_source;
    using yaddnsc_detail::stop_callback;
    using yaddnsc_detail::jthread;
} // namespace std

#endif // defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L

#endif //YADDNSC_STOP_TOKEN_COMPAT_H
