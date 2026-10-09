#ifndef YADDNSC_SUPPORT_UTIL_FD_HPP
#define YADDNSC_SUPPORT_UTIL_FD_HPP

#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

// pipe2() is available on Linux and the BSDs (FreeBSD >= 10, NetBSD >= 6,
// OpenBSD >= 5.7, DragonFly >= 4.2).  macOS does not have it.
//
// NOTE: on glibc/musl, pipe2() is declared in <unistd.h> only when
// _GNU_SOURCE is defined.  g++/clang++ on Linux define it by default, so
// normally nothing is needed here; if your build disables GNU extensions,
// define _GNU_SOURCE before including this header.
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define YADDNSC_HAVE_PIPE2 1
#endif

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

namespace detail {

/// Portable fallback used when pipe2() is unavailable (e.g. macOS).
/// Creates a pipe and sets O_CLOEXEC + O_NONBLOCK on both ends.
/// On success returns 0 and fills fds[0]/fds[1]; on failure returns -1
/// (errno set) and leaves both fds set to -1.
inline int make_pipe_fcntl(int fds[2]) noexcept {
    if (::pipe(fds) != 0) {
        return -1;
    }

    const auto setup = [](int fd) -> bool {
        // FD_CLOEXEC is the only valid descriptor flag for F_SETFD.
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
            return false;
        }
        // O_NONBLOCK is a *status* flag: F_SETFL replaces the whole set,
        // so read the current flags first and OR it in.
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            return false;
        }
        return true;
    };

    if (setup(fds[0]) && setup(fds[1])) {
        return 0;
    }

    const int error = errno;
    [[maybe_unused]] const auto close_read = ::close(fds[0]);
    [[maybe_unused]] const auto close_write = ::close(fds[1]);
    fds[0] = -1;
    fds[1] = -1;
    errno = error;
    return -1;
}

}  // namespace detail

/// Create a non-blocking pipe (see pipe(2)) and return both ends as
/// RAII wrappers.
///
/// Both ends have O_NONBLOCK and close-on-exec set. Non-blocking keeps
/// cancellation signalling safe in noexcept paths: the single latch byte
/// never blocks a trigger, and cancellation readers only poll it (they
/// never consume the byte).
///
/// Returns a pair of (read_end, write_end).
/// On failure, both fds are invalid (operator bool returns false for both).
[[nodiscard]] inline std::pair<UniqueFd, UniqueFd> make_pipe() noexcept {
    int fds[2] = {-1, -1};

#ifdef YADDNSC_HAVE_PIPE2
    // Atomic on Linux/BSD: no window where a fork()ed child could inherit
    // the fds without O_CLOEXEC.
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        fds[0] = -1;
        fds[1] = -1;
    }
#else
    if (detail::make_pipe_fcntl(fds) != 0) {
        fds[0] = -1;
        fds[1] = -1;
    }
#endif

    return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}
}  // namespace Utils

#endif  // YADDNSC_SUPPORT_UTIL_FD_HPP
