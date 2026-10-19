//
// net — TlsStream implementation.
//

#include "stream.h"

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <csignal>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <spdlog/spdlog.h>

#include "coro/cancelled.h"
#include "coro/checkpoint.hpp"
#include "coro/fd_wait.hpp"
#include "infrastructure/logging/level_enabled.h"
#include "infrastructure/network/tls/context.h"
#include "infrastructure/network/tls/openssl_error.hpp"

namespace net {
namespace {

/// OpenSSL wants an int length; clamp rather than narrow silently.
[[nodiscard]] int openssl_length(const std::size_t size) noexcept {
    constexpr auto LIMIT = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return size > LIMIT ? std::numeric_limits<int>::max() : static_cast<int>(size);
}

/// How this connection identifies its peer.
///
/// RFC 6066 §3 forbids an IP literal in SNI, so a literal is a verification
/// identity only, while a configured name is both. When no name is configured
/// the dialed address is the identity, and in this stage that address is always
/// an IP literal.
struct VerificationIdentity {
    std::string name;  ///< the text OpenSSL receives: a hostname or an address literal
    bool is_literal{false};
};

/// Pick the peer's verification identity once: a pinned name is parsed a single
/// time, and an unpinned connection falls back to its own target address.
[[nodiscard]] VerificationIdentity verification_identity(const TlsOptions& options, const domain::InetAddress& target) {
    if (options.sni_hostname.has_value() && !options.sni_hostname->empty()) {
        if (const auto literal = domain::InetAddress::parse(*options.sni_hostname)) {
            return {.name = literal->to_string(), .is_literal = true};
        }
        return {.name = *options.sni_hostname, .is_literal = false};
    }
    return {.name = target.to_string(), .is_literal = true};
}

/// Send the SNI extension and pin the certificate identity. RFC 6066 §3: an IP
/// literal never reaches SSL_set_tlsext_host_name().
[[nodiscard]] std::expected<void, IoError> apply_verification_identity(SSL* ssl, X509_VERIFY_PARAM* params,
                                                                       const VerificationIdentity& identity) {
    if (!identity.is_literal) {
        if (SSL_set_tlsext_host_name(ssl, identity.name.c_str()) != 1) {
            SPDLOG_ERROR(R"(Failed to set SNI "{}": {})", identity.name, detail::ssl_errors());
            return std::unexpected(IoError::TLS_HANDSHAKE_FAILED);
        }
        // OpenSSL 4 deprecates SSL_set1_host. A zero length means the name is
        // NUL-terminated.
        if (X509_VERIFY_PARAM_set1_host(params, identity.name.c_str(), 0) != 1) {
            SPDLOG_ERROR(R"(Failed to set hostname verification for "{}": {})", identity.name, detail::ssl_errors());
            return std::unexpected(IoError::TLS_HANDSHAKE_FAILED);
        }
        return {};
    }
    if (X509_VERIFY_PARAM_set1_ip_asc(params, identity.name.c_str()) != 1) {
        SPDLOG_ERROR(R"(Failed to set IP verification for "{}": {})", identity.name, detail::ssl_errors());
        return std::unexpected(IoError::TLS_HANDSHAKE_FAILED);
    }
    return {};
}

/// Offer the configured ALPN protocols. An empty list leaves the wire alone.
[[nodiscard]] std::expected<void, IoError> apply_alpn(SSL* ssl, const std::span<const unsigned char> protocols) {
    if (protocols.empty()) {
        return {};
    }
    if (protocols.size() > std::numeric_limits<unsigned int>::max() ||
        SSL_set_alpn_protos(ssl, protocols.data(), static_cast<unsigned int>(protocols.size())) != 0) {
        SPDLOG_ERROR("SSL_set_alpn_protos failed: {}", detail::ssl_errors());
        return std::unexpected(IoError::TLS_HANDSHAKE_FAILED);
    }
    return {};
}

/// The direction the last failed SSL call asked for: WANT_WRITE stays POLLOUT,
/// every other retryable WANT_* this layer handles is POLLIN.
[[nodiscard]] auto direction_wait(const int fd, const int ssl_error) noexcept {
    return ssl_error == SSL_ERROR_WANT_WRITE ? coro::wait_writable(fd) : coro::wait_readable(fd);
}

}  // namespace

namespace {

/// Runs one OpenSSL call with SIGPIPE blocked.
///
/// OpenSSL writes to the socket with write(2), which raises SIGPIPE once the
/// peer has gone away — that would kill the process, and a transport must never
/// do that on a peer reset. MSG_NOSIGNAL is not reachable through SSL_set_fd and
/// SO_NOSIGPIPE does not exist on Linux, so the signal is blocked for the call
/// and a pending instance is consumed before unblocking. The scope is one call,
/// never a suspension, so no other coroutine runs with the signal blocked.
class SigpipeBlocker {
public:
    SigpipeBlocker() noexcept {
        sigset_t blocked{};
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        active_ = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous_) == 0;
    }

    SigpipeBlocker(const SigpipeBlocker&) = delete;
    SigpipeBlocker& operator=(const SigpipeBlocker&) = delete;

    ~SigpipeBlocker() {
        if (!active_) {
            return;
        }
        sigset_t pending{};
        if (::sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
            // Consume the pending instance before unblocking, so it never
            // reaches the default disposition. sigwait is the portable
            // sigtimedwait: macOS lacks the latter, and there sigismember is
            // a macro (no :: qualification). It cannot block here — the
            // signal is pending and still blocked — and a SIGPIPE-only set
            // leaves other pending signals to their handlers.
            sigset_t pipe_only{};
            sigemptyset(&pipe_only);
            sigaddset(&pipe_only, SIGPIPE);
            int consumed = 0;
            (void) ::sigwait(&pipe_only, &consumed);
        }
        ::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

private:
    sigset_t previous_{};
    bool active_ = false;
};

/// Run `fn` under the SIGPIPE blocker and return its result.
template<typename Fn>
[[nodiscard]] decltype(auto) without_sigpipe(Fn&& fn) {
    const SigpipeBlocker blocker;
    return std::forward<Fn>(fn)();
}

}  // namespace

void SslDeleter::operator()(SSL* ssl) const noexcept {
    SSL_free(ssl);
}

TlsStream::TlsStream(domain::InetAddress address, const std::uint16_t port, std::shared_ptr<const TlsContext> context,
                     ConnectOptions options, TlsOptions tls_options)
    : tcp_(address, port, std::move(options)), tls_options_(std::move(tls_options)),
      alpn_proto_(tls_options_.alpn_proto.begin(), tls_options_.alpn_proto.end()), context_(std::move(context)) {}

TlsStream::~TlsStream() {
    close();
}

coro::Task<std::expected<void, IoError>> TlsStream::ensure_connected() {
    co_await coro::checkpoint();
    if (tcp_.connected() && ssl_ != nullptr) {
        co_return {};
    }
    close();

    if (auto connected = co_await tcp_.ensure_connected(); !connected) {
        co_return std::unexpected(connected.error());
    }
    if (auto prepared = prepare_session(); !prepared) {
        close();
        co_return std::unexpected(prepared.error());
    }
    try {
        if (auto handshaken = co_await handshake(); !handshaken) {
            close();
            co_return std::unexpected(handshaken.error());
        }
    } catch (const coro::Cancelled&) {
        close();
        throw;
    }
    if (logging::level_enabled(spdlog::level::debug)) {
        // peer_label() allocates twice — build it only for a record that is written.
        SPDLOG_DEBUG(R"(TLS session established with "{}" ({}))", peer_label(), SSL_get_version(ssl_.get()));
    }
    co_return {};
}

std::string TlsStream::peer_label() const {
    // The TLS identity when one is pinned, otherwise the connection target.
    const std::string host = tls_options_.sni_hostname.value_or(tcp_.address().to_string());
    return host + ':' + std::to_string(tcp_.port());
}

std::expected<void, IoError> TlsStream::prepare_session() {
    if (context_ == nullptr) {
        SPDLOG_ERROR("TLS stream has no trust context (build one off-loop with TlsContext::create)");
        return std::unexpected(IoError::TLS_HANDSHAKE_FAILED);
    }

    SslPtr ssl{SSL_new(context_->native_handle())};
    if (!ssl) {
        SPDLOG_ERROR("SSL_new failed: {}", detail::ssl_errors());
        return std::unexpected(IoError::TLS_HANDSHAKE_FAILED);
    }
    SSL_set_fd(ssl.get(), tcp_.native_handle());
    SSL_set_connect_state(ssl.get());

    if (auto identified = apply_verification_identity(ssl.get(), SSL_get0_param(ssl.get()),
                                                      verification_identity(tls_options_, tcp_.address()));
        !identified) {
        return identified;
    }
    if (auto offered = apply_alpn(ssl.get(), alpn_proto_); !offered) {
        return offered;
    }

    ssl_ = std::move(ssl);
    return {};
}

coro::Task<std::expected<void, IoError>> TlsStream::handshake() {
    co_await coro::checkpoint();
    for (;;) {
        const int result = without_sigpipe([this] { return SSL_connect(ssl_.get()); });
        if (result == 1) {
            co_return {};
        }
        const int error = SSL_get_error(ssl_.get(), result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            // A verification or handshake failure is worth an ERROR with the
            // peer's identity.
            SPDLOG_ERROR(R"(TLS handshake failed for "{}": {})", peer_label(), detail::ssl_errors());
            co_return std::unexpected(IoError::TLS_HANDSHAKE_FAILED);
        }
        if (const std::error_code wait = co_await direction_wait(tcp_.native_handle(), error)) {
            SPDLOG_DEBUG("TLS handshake wait failed: {}", wait.message());
            co_return std::unexpected(IoError::CONNECTION_FAILED);
        }
    }
}

coro::Task<std::expected<std::size_t, IoError>> TlsStream::read_some(std::span<std::uint8_t> buf) {
    co_await coro::checkpoint();
    if (buf.empty()) {
        co_return 0;
    }
    if (ssl_ == nullptr) {
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    for (;;) {
        const int received =
            without_sigpipe([&] { return SSL_read(ssl_.get(), buf.data(), openssl_length(buf.size())); });
        if (received > 0) {
            co_return static_cast<std::size_t>(received);
        }
        const int error = SSL_get_error(ssl_.get(), received);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            if (const std::error_code wait = co_await direction_wait(tcp_.native_handle(), error)) {
                SPDLOG_DEBUG("TLS fd wait failed: {}", wait.message());
                co_return std::unexpected(IoError::CONNECTION_FAILED);
            }
            continue;
        }
        if (logging::level_enabled(spdlog::level::debug)) {
            SPDLOG_DEBUG("TLS read failed: {}", detail::ssl_errors());
        }
        co_return std::unexpected(IoError::CONNECTION_FAILED);  // includes a clean shutdown
    }
}

coro::Task<std::expected<void, IoError>> TlsStream::read_exact(std::span<std::uint8_t> buf) {
    co_await coro::checkpoint();
    auto remaining = buf;
    while (!remaining.empty()) {
        auto received = co_await read_some(remaining);
        if (!received) {
            co_return std::unexpected(received.error());
        }
        remaining = remaining.subspan(*received);
    }
    co_return {};
}

coro::Task<std::expected<void, IoError>> TlsStream::send_all(std::span<const std::uint8_t> data) {
    co_await coro::checkpoint();
    if (data.empty()) {
        co_return {};
    }
    if (ssl_ == nullptr) {
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    auto remaining = data;
    while (!remaining.empty()) {
        const int sent =
            without_sigpipe([&] { return SSL_write(ssl_.get(), remaining.data(), openssl_length(remaining.size())); });
        if (sent > 0) {
            remaining = remaining.subspan(static_cast<std::size_t>(sent));
            continue;
        }
        const int error = SSL_get_error(ssl_.get(), sent);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            if (const std::error_code wait = co_await direction_wait(tcp_.native_handle(), error)) {
                SPDLOG_DEBUG("TLS fd wait failed: {}", wait.message());
                co_return std::unexpected(IoError::CONNECTION_FAILED);
            }
            continue;
        }
        if (logging::level_enabled(spdlog::level::debug)) {
            SPDLOG_DEBUG("TLS write failed: {}", detail::ssl_errors());
        }
        co_return std::unexpected(IoError::CONNECTION_FAILED);
    }
    co_return {};
}

void TlsStream::close() noexcept {
    // SSL_free before the socket it borrows is closed.
    ssl_.reset();
    tcp_.close();
}

bool TlsStream::connected() const noexcept {
    return ssl_ != nullptr && tcp_.connected();
}

}  // namespace net
