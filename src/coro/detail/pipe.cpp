//
// Coroutine runtime — the wake-pipe factory.
//

#include "pipe.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

// pipe2() is available on Linux and the BSDs (FreeBSD >= 10, NetBSD >= 6,
// OpenBSD >= 5.7, DragonFly >= 4.2).  macOS does not have it.
//
// NOTE: on glibc/musl, pipe2() is declared in <unistd.h> only when
// _GNU_SOURCE is defined.  g++/clang++ on Linux define it by default, so
// normally nothing is needed here; if your build disables GNU extensions,
// define _GNU_SOURCE before including this file.
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define YADDNSC_HAVE_PIPE2 1
#endif

namespace coro::detail {
namespace {

#ifndef YADDNSC_HAVE_PIPE2
/// Portable fallback used when pipe2() is unavailable (e.g. macOS).
/// Creates a pipe and sets O_CLOEXEC + O_NONBLOCK on both ends.
/// On success returns 0 and fills fds[0]/fds[1]; on failure returns -1
/// (errno set) and leaves both fds set to -1.
int make_pipe_fcntl(int fds[2]) noexcept {
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
#endif  // !YADDNSC_HAVE_PIPE2

}  // namespace

std::pair<Utils::UniqueFd, Utils::UniqueFd> make_pipe() noexcept {
    int fds[2] = {-1, -1};

#ifdef YADDNSC_HAVE_PIPE2
    // Atomic on Linux/BSD: no window where a fork()ed child could inherit
    // the fds without O_CLOEXEC.
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        fds[0] = -1;
        fds[1] = -1;
    }
#else
    if (make_pipe_fcntl(fds) != 0) {
        fds[0] = -1;
        fds[1] = -1;
    }
#endif

    return {Utils::UniqueFd(fds[0]), Utils::UniqueFd(fds[1])};
}

}  // namespace coro::detail
