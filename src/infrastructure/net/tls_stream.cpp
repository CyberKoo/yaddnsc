//
// net — TlsStream implementation.
//

#include "tls_stream.h"

#include <array>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <pthread.h>
#include <signal.h>
#include <spdlog/spdlog.h>

#include "infrastructure/coro/fd_wait.hpp"
#include "infrastructure/net/tls/cert_util.h"

namespace net {
namespace {

/// OpenSSL wants an int length; clamp rather than narrow silently.
[[nodiscard]] int openssl_length(const std::size_t size) noexcept {
    constexpr auto LIMIT = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return size > LIMIT ? std::numeric_limits<int>::max() : static_cast<int>(size);
}

/// The OpenSSL error stack as one line, for diagnostics only.
[[nodiscard]] std::string ssl_errors() {
    std::string text;
    unsigned long error = 0;
    while ((error = ERR_get_error()) != 0) {
        std::array<char, 256> buffer{};
        ERR_error_string_n(error, buffer.data(), buffer.size());
        if (!text.empty()) {
            text.append("; ");
        }
        text.append(buffer.data());
    }
    return text;
}

/// The textual form of an address, without any IPv6 scope id.
[[nodiscard]] std::string ip_literal(const InetAddress& address) {
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    const int family = address.get_family() == AddressFamily::IPV6 ? AF_INET6 : AF_INET;
    if (::inet_ntop(family, address.get_address().data(), buffer.data(), buffer.size()) == nullptr) {
        return {};
    }
    std::string text{buffer.data()};
    if (const auto percent = text.find('%'); percent != std::string::npos) {
        text.erase(percent);  // scope id is not part of the address
    }
    return text;
}

/// The direction the last failed SSL call asked for: WANT_WRITE stays POLLOUT,
/// every other retryable WANT_* this layer handles is POLLIN.
[[nodiscard]] coro::FdAwaitable direction_wait(const int fd, const int ssl_error) noexcept {
    return ssl_error == SSL_ERROR_WANT_WRITE ? coro::FdAwaitable::writable(fd) : coro::FdAwaitable::readable(fd);
}

/// Build the client SSL_CTX for @p options. Verification is fail-closed.
[[nodiscard]] SslCtxPtr build_context(const TlsOptions& options) {
    SslCtxPtr context{SSL_CTX_new(TLS_client_method())};
    if (!context) {
        SPDLOG_ERROR("SSL_CTX_new failed: {}", ssl_errors());
        return nullptr;
    }

    if (SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) != 1) {
        SPDLOG_ERROR("Failed to restrict TLS versions: {}", ssl_errors());
        return nullptr;
    }

    if (!options.verify_peer) {
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
        return context;
    }

    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);

    // CA: explicit path -> discovery -> OpenSSL defaults (fail-closed).
    std::optional<std::string> ca_path = options.ca_bundle;
    if (!ca_path) {
        ca_path = Utils::Cert::discover_ca_bundle();
    }
    if (ca_path) {
        if (SSL_CTX_load_verify_locations(context.get(), ca_path->c_str(), nullptr) != 1) {
            SPDLOG_ERROR("Failed to load CA bundle from {}: {}", *ca_path, ssl_errors());
            return nullptr;
        }
    } else if (SSL_CTX_set_default_verify_paths(context.get()) != 1) {
        SPDLOG_ERROR("No CA bundle found and OpenSSL default paths failed: {}", ssl_errors());
        return nullptr;
    }
    return context;
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
        if (::sigpending(&pending) == 0 && ::sigismember(&pending, SIGPIPE) == 1) {
            const timespec immediate{0, 0};
            ::sigtimedwait(&pending, nullptr, &immediate);
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

void SslContextDeleter::operator()(SSL_CTX* ctx) const noexcept {
    SSL_CTX_free(ctx);
}

void SslDeleter::operator()(SSL* ssl) const noexcept {
    SSL_free(ssl);
}

TlsStream::TlsStream(InetAddress address, const std::uint16_t port, ConnectOptions options, TlsOptions tls_options)
    : tcp_(std::move(address), port, std::move(options)), tls_options_(std::move(tls_options)),
      alpn_proto_(tls_options_.alpn_proto.begin(), tls_options_.alpn_proto.end()) {}

TlsStream::~TlsStream() {
    close();
}

coro::Task<std::expected<void, IoError>> TlsStream::ensure_connected() {
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
    if (auto handshaken = co_await handshake(); !handshaken) {
        close();
        co_return std::unexpected(handshaken.error());
    }
    SPDLOG_DEBUG("TLS session established with {}:{} ({})", ip_literal(tcp_.address()), tcp_.port(),
                 SSL_get_version(ssl_.get()));
    co_return {};
}

std::expected<void, IoError> TlsStream::prepare_session() {
    if (!context_) {
        context_ = build_context(tls_options_);
        if (!context_) {
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
    }

    SslPtr ssl{SSL_new(context_.get())};
    if (!ssl) {
        SPDLOG_ERROR("SSL_new failed: {}", ssl_errors());
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    SSL_set_fd(ssl.get(), tcp_.native_handle());
    SSL_set_connect_state(ssl.get());

    X509_VERIFY_PARAM* verify_param = SSL_get0_param(ssl.get());
    const bool has_name = tls_options_.sni_hostname.has_value() && !tls_options_.sni_hostname->empty();

    if (has_name) {
        const std::string& name = *tls_options_.sni_hostname;
        const bool is_ip = InetAddress::parse(name).has_value();
        if (!is_ip && SSL_set_tlsext_host_name(ssl.get(), name.c_str()) != 1) {
            SPDLOG_ERROR(R"(Failed to set SNI "{}": {})", name, ssl_errors());
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
        if (is_ip) {
            const std::string literal = ip_literal(*InetAddress::parse(name));
            if (literal.empty() || X509_VERIFY_PARAM_set1_ip_asc(verify_param, literal.c_str()) != 1) {
                SPDLOG_ERROR(R"(Failed to set IP verification for "{}": {})", name, ssl_errors());
                return std::unexpected(IoError::CONNECTION_FAILED);
            }
        } else if (X509_VERIFY_PARAM_set1_host(verify_param, name.c_str(), 0) != 1) {
            // OpenSSL 4 deprecates SSL_set1_host. A zero length means the name is
            // NUL-terminated.
            SPDLOG_ERROR(R"(Failed to set hostname verification for "{}": {})", name, ssl_errors());
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
    } else {
        // No explicit name: the target is the verification identity, and it is
        // an IP literal in this stage.
        const std::string literal = ip_literal(tcp_.address());
        if (literal.empty() || X509_VERIFY_PARAM_set1_ip_asc(verify_param, literal.c_str()) != 1) {
            SPDLOG_ERROR(R"(Failed to set IP verification for "{}": {})", literal, ssl_errors());
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
    }

    if (!alpn_proto_.empty()) {
        if (alpn_proto_.size() > std::numeric_limits<unsigned int>::max() ||
            SSL_set_alpn_protos(ssl.get(), alpn_proto_.data(), static_cast<unsigned int>(alpn_proto_.size())) != 0) {
            SPDLOG_ERROR("SSL_set_alpn_protos failed: {}", ssl_errors());
            return std::unexpected(IoError::CONNECTION_FAILED);
        }
    }

    ssl_ = std::move(ssl);
    return {};
}

coro::Task<std::expected<void, IoError>> TlsStream::handshake() {
    for (;;) {
        const int result = without_sigpipe([this] { return SSL_connect(ssl_.get()); });
        if (result == 1) {
            co_return {};
        }
        const int error = SSL_get_error(ssl_.get(), result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            SPDLOG_DEBUG("TLS handshake failed: {}", ssl_errors());
            co_return std::unexpected(IoError::CONNECTION_FAILED);
        }
        if (auto ready = co_await direction_wait(tcp_.native_handle(), error); !ready) {
            co_return std::unexpected(IoError::CANCELLED);
        }
    }
}

coro::Task<std::expected<std::size_t, IoError>> TlsStream::read_some(std::span<std::uint8_t> buf) {
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
            if (auto ready = co_await direction_wait(tcp_.native_handle(), error); !ready) {
                co_return std::unexpected(IoError::CANCELLED);
            }
            continue;
        }
        SPDLOG_DEBUG("TLS read failed: {}", ssl_errors());
        co_return std::unexpected(IoError::CONNECTION_FAILED);  // includes a clean shutdown
    }
}

coro::Task<std::expected<void, IoError>> TlsStream::read_exact(std::span<std::uint8_t> buf) {
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
            if (auto ready = co_await direction_wait(tcp_.native_handle(), error); !ready) {
                co_return std::unexpected(IoError::CANCELLED);
            }
            continue;
        }
        SPDLOG_DEBUG("TLS write failed: {}", ssl_errors());
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
