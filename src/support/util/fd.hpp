#ifndef YADDNSC_SUPPORT_UTIL_FD_HPP
#define YADDNSC_SUPPORT_UTIL_FD_HPP

#include <utility>

#include <unistd.h>

namespace Utils {
/// RAII wrapper for a file descriptor.
///
/// Automatically closes the fd on destruction. Move-only — no copies.
/// Default-constructs with fd = -1 (invalid/closed state).
///
/// @code
///   Utils::UniqueFd fd(::open(...));
///   // ... use fd.get() ...
///   // fd closes automatically when it goes out of scope.
///
///   auto fd2 = std::move(fd);  // transfer ownership
/// @endcode
class UniqueFd {
public:
    UniqueFd() noexcept = default;

    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() { close(); }

    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    UniqueFd(const UniqueFd&) = delete;

    UniqueFd& operator=(const UniqueFd&) = delete;

    /// Close the current fd (if any) and take ownership of a new one.
    /// Passing the fd number currently owned is a no-op (guards against
    /// self-reset, which would otherwise close then re-store a dead fd).
    void reset(int fd = -1) noexcept {
        if (fd == fd_) {
            return;
        }
        close();
        fd_ = fd;
    }

    /// Release ownership without closing the fd.
    /// Returns the fd number; the caller is responsible for closing it.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    /// Return the raw fd number (-1 if closed).
    [[nodiscard]] int get() const noexcept { return fd_; }

    /// True if this object owns a valid fd (>= 0).
    explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    void close() noexcept {
        if (fd_ >= 0) {
            const int old_fd = fd_;
            fd_ = -1;  // mark closed before calling ::close to prevent double-close
            [[maybe_unused]] auto _ = ::close(old_fd);
        }
    }

    int fd_ = -1;
};

}  // namespace Utils

#endif  // YADDNSC_SUPPORT_UTIL_FD_HPP
