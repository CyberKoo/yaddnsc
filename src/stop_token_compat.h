//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_STOP_TOKEN_COMPAT_H
#define YADDNSC_STOP_TOKEN_COMPAT_H

// Drop-in compatibility header for std::stop_token, std::stop_source, and
// std::jthread.  Uses the native standard-library implementations when
// available (Apple Clang >= 15 / Xcode >= 15, GCC >= 11, Clang >= 14,
// MSVC >= 2022 17.4) and falls back to a minimal polyfill otherwise.

#include <version>

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L

#include <stop_token>
#include <thread>

#else  // no native support – provide a minimal polyfill

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

// -----------------------------------------------------------------------
// Minimal polyfill for std::stop_token / std::stop_source / std::jthread
// -----------------------------------------------------------------------
// These are simplified implementations that cover only the subset of the
// API actually used by yaddnsc.  They are *not* intended to be full
// conforming replacements.

namespace yaddnsc_detail {

class stop_token {
public:
    stop_token() = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stop_possible() const noexcept {
        return flag_ != nullptr;
    }

private:
    friend class stop_source;

    explicit stop_token(std::shared_ptr<std::atomic<bool>> flag)
        : flag_(std::move(flag)) {}

    std::shared_ptr<std::atomic<bool>> flag_;
};

class stop_source {
public:
    stop_source()
        : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    stop_source(const stop_source &) = default;
    stop_source(stop_source &&) noexcept = default;
    stop_source &operator=(const stop_source &) = default;
    stop_source &operator=(stop_source &&) noexcept = default;

    [[nodiscard]] stop_token get_token() const noexcept {
        return stop_token(flag_);
    }

    [[nodiscard]] bool stop_possible() const noexcept {
        return true;
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

    bool request_stop() noexcept {
        if (flag_) {
            flag_->store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
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
    using yaddnsc_detail::jthread;
} // namespace std

#endif // defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L

#endif //YADDNSC_STOP_TOKEN_COMPAT_H
