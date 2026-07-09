//
// Created by Kotarou on 2026/7/8.
//

#ifndef YADDNSC_UTIL_FD_H
#define YADDNSC_UTIL_FD_H

#include <utility>

#include <fcntl.h>
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

        explicit UniqueFd(int fd) noexcept : fd_(fd) {
        }

        ~UniqueFd() {
            close();
        }

        UniqueFd(UniqueFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {
        }

        UniqueFd &operator=(UniqueFd &&other) noexcept {
            if (this != &other) {
                close();
                fd_ = std::exchange(other.fd_, -1);
            }
            return *this;
        }

        UniqueFd(const UniqueFd &) = delete;

        UniqueFd &operator=(const UniqueFd &) = delete;

        /// Close the current fd (if any) and take ownership of a new one.
        void reset(int fd = -1) noexcept {
            close();
            fd_ = fd;
        }

        /// Release ownership without closing the fd.
        /// Returns the fd number; the caller is responsible for closing it.
        [[nodiscard]] int release() noexcept {
            return std::exchange(fd_, -1);
        }

        /// Return the raw fd number (-1 if closed).
        [[nodiscard]] int get() const noexcept {
            return fd_;
        }

        /// True if this object owns a valid fd (>= 0).
        explicit operator bool() const noexcept {
            return fd_ >= 0;
        }

    private:
        void close() noexcept {
            if (fd_ >= 0) {
                const int old_fd = fd_;
                fd_ = -1; // mark closed before calling ::close to prevent double-close
                [[maybe_unused]] auto _ = ::close(old_fd);
            }
        }

        int fd_ = -1;
    };

    /// Create a pipe (see pipe(2)) and return both ends as RAII wrappers.
    ///
    /// Returns a pair of (read_end, write_end).
    /// On failure, both fds are invalid (operator bool returns false for both).
    [[nodiscard]] inline std::pair<UniqueFd, UniqueFd> make_pipe() noexcept {
        int fds[2] = {-1, -1};
        if (::pipe(fds) == 0) {
            // Set close-on-exec for both ends.
            ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        }
        return {UniqueFd(fds[0]), UniqueFd(fds[1])};
    }
} // namespace Utils

#endif  // YADDNSC_UTIL_FD_H
