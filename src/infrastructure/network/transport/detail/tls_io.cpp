//
// Direction-correct TLS read and write.
//
#include "tls_io.h"

#include <array>
#include <cerrno>
#include <limits>
#include <string>
#include <vector>

#include <openssl/err.h>
#include <poll.h>
#include <spdlog/spdlog.h>

#include "infrastructure/network/socket.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

namespace Transport::detail {
namespace {

[[nodiscard]] int openssl_length(const std::size_t n) noexcept {
    constexpr auto LIMIT = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (n > LIMIT) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(n);
}

[[nodiscard]] std::expected<void, IoError> map_wait(const std::expected<short, int>& ready) {
    if (ready) {
        return {};
    }
    if (ready.error() == ETIMEDOUT) {
        return std::unexpected(IoError::TIMEOUT);
    }
    if (ready.error() == ECANCELED) {
        return std::unexpected(IoError::CANCELLED);
    }
    return std::unexpected(IoError::CONNECTION_FAILED);
}

/// WANT_WRITE stays POLLOUT. Every other WANT_* this layer retries is POLLIN.
[[nodiscard]] short direction_events(const int ssl_error) noexcept {
    return ssl_error == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN;
}

[[nodiscard]] bool spent(const std::chrono::steady_clock::time_point deadline) noexcept {
    return std::chrono::steady_clock::now() >= deadline;
}

}  // namespace

std::string tls_error_text() {
    std::vector<std::string> errors;
    unsigned long err = 0;
    while ((err = ERR_get_error()) != 0) {
        std::array<char, 256> buf{};
        ERR_error_string_n(err, buf.data(), buf.size());
        errors.emplace_back(buf.data());
    }
    return fmt::format("{}", fmt::join(errors, "; "));
}

std::expected<void, IoError> wait_for_tls_direction(const int ssl_error, Socket& socket,
                                                    const std::chrono::steady_clock::time_point deadline,
                                                    const Utils::CancellationToken& token) {
    if (spent(deadline)) {
        return std::unexpected(IoError::TIMEOUT);
    }
    auto ready = socket.wait_until(direction_events(ssl_error), deadline, token);
    return map_wait(ready);
}

std::expected<std::size_t, IoError> read_ssl(SSL* ssl, Socket& socket, const std::span<std::uint8_t> buf,
                                             const std::chrono::steady_clock::time_point deadline,
                                             const Utils::CancellationToken& token) {
    using enum IoError;

    for (;;) {
        if (token.is_triggered()) {
            return std::unexpected(CANCELLED);
        }

        // Decrypted bytes already held by OpenSSL are not a new socket read.
        const bool buffered = SSL_pending(ssl) > 0;
        if (!buffered && spent(deadline)) {
            return std::unexpected(TIMEOUT);
        }

        const int rc = SSL_read(ssl, buf.data(), openssl_length(buf.size()));
        if (rc > 0) {
            return static_cast<std::size_t>(rc);
        }

        const int err = SSL_get_error(ssl, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            auto waited = wait_for_tls_direction(err, socket, deadline, token);
            if (!waited) {
                return std::unexpected(waited.error());
            }
            continue;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return std::unexpected(CONNECTION_FAILED);
        }
        SPDLOG_DEBUG("TLS read failed: {}", tls_error_text());
        return std::unexpected(CONNECTION_FAILED);
    }
}

std::expected<void, IoError> write_ssl(SSL* ssl, Socket& socket, const std::span<const std::uint8_t> data,
                                       const std::chrono::steady_clock::time_point deadline,
                                       const Utils::CancellationToken& token) {
    using enum IoError;

    auto remaining = data;
    while (!remaining.empty()) {
        if (token.is_triggered()) {
            return std::unexpected(CANCELLED);
        }
        if (spent(deadline)) {
            return std::unexpected(TIMEOUT);
        }

        const int rc = SSL_write(ssl, remaining.data(), openssl_length(remaining.size()));
        if (rc > 0) {
            remaining = remaining.subspan(static_cast<std::size_t>(rc));
            continue;
        }

        const int err = SSL_get_error(ssl, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            auto waited = wait_for_tls_direction(err, socket, deadline, token);
            if (!waited) {
                return std::unexpected(waited.error());
            }
            continue;
        }
        SPDLOG_DEBUG("TLS write failed: {}", tls_error_text());
        return std::unexpected(CONNECTION_FAILED);
    }
    return {};
}

}  // namespace Transport::detail
