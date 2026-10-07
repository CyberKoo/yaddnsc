//
// Deadline-bounded TCP byte transfer on an already-connected Socket.
//
#include "tcp_transfer.h"

#include <cerrno>
#include <utility>

#include <poll.h>

#include "infrastructure/network/socket.h"
#include "support/util/cancellation_token.hpp"

namespace {

[[nodiscard]] bool retryable_io(const int errnum) noexcept {
    return errnum == EINTR || errnum == EAGAIN || errnum == EWOULDBLOCK;
}

[[nodiscard]] bool spent(const std::chrono::steady_clock::time_point deadline) noexcept {
    return std::chrono::steady_clock::now() >= deadline;
}

}  // namespace

std::expected<std::size_t, int> tcp_read_some(Socket& socket, const std::span<std::byte> buf,
                                              const std::chrono::steady_clock::time_point deadline,
                                              const Utils::CancellationToken& token) noexcept {
    if (buf.empty()) {
        return 0;
    }

    for (;;) {
        if (token.is_triggered()) {
            return std::unexpected(ECANCELED);
        }
        // Past the deadline: do not poll and do not recv. A readable socket
        // is not permission to continue the transfer.
        if (spent(deadline)) {
            return std::unexpected(ETIMEDOUT);
        }

        auto ready = socket.wait_until(POLLIN, deadline, token);
        if (!ready) {
            return std::unexpected(ready.error());
        }
        if (spent(deadline)) {
            return std::unexpected(ETIMEDOUT);
        }

        auto n = socket.recv_some(buf);
        if (!n) {
            if (retryable_io(n.error())) {
                continue;
            }
            return std::unexpected(n.error());
        }
        return *n;
    }
}

std::expected<void, int> tcp_read_exact(Socket& socket, const std::span<std::byte> buf,
                                        const std::chrono::steady_clock::time_point deadline,
                                        const Utils::CancellationToken& token) noexcept {
    auto remaining = buf;
    while (!remaining.empty()) {
        auto n = tcp_read_some(socket, remaining, deadline, token);
        if (!n) {
            return std::unexpected(n.error());
        }
        if (*n == 0) {
            return std::unexpected(ECONNRESET);
        }
        remaining = remaining.subspan(*n);
    }
    return {};
}

std::expected<void, int> tcp_send_all(Socket& socket, const std::span<const std::byte> data,
                                      const std::chrono::steady_clock::time_point deadline,
                                      const Utils::CancellationToken& token) noexcept {
    auto remaining = data;
    while (!remaining.empty()) {
        if (token.is_triggered()) {
            return std::unexpected(ECANCELED);
        }
        if (spent(deadline)) {
            return std::unexpected(ETIMEDOUT);
        }

        auto ready = socket.wait_until(POLLOUT, deadline, token);
        if (!ready) {
            return std::unexpected(ready.error());
        }
        if (spent(deadline)) {
            return std::unexpected(ETIMEDOUT);
        }

        auto n = socket.send_some(remaining);
        if (!n) {
            if (retryable_io(n.error())) {
                continue;
            }
            return std::unexpected(n.error());
        }
        if (*n == 0) {
            continue;
        }
        remaining = remaining.subspan(*n);
    }
    return {};
}
