//
// TlsStream — self-managing TLS byte stream (Transport).
//
#include "infrastructure/network/transport/tls_stream.h"

#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <openssl/prov_ssl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>

#include "domain/network/inet_address.h"
#include "infrastructure/network/tls/cert_util.h"
#include "infrastructure/network/transport/detail/tls_io.h"
#include "support/util/cancellation_token.hpp"

namespace Transport {
namespace {

/// Format the OpenSSL error stack for logging.
[[nodiscard]] std::string ssl_errors() {
    return detail::tls_error_text();
}

/// Build an SSL_CTX for the given options.
///
/// Shared default (verify-on, auto-discovered CA) is used when the
/// options match it; otherwise a per-instance ctx is built (custom CA
/// bundle or verification disabled).
[[nodiscard]] SslCtxPtr make_ssl_ctx(const TlsOptions& opts) {
    SslCtxPtr ctx(SSL_CTX_new(TLS_client_method()));
    if (!ctx) {
        SPDLOG_ERROR("SSL_CTX_new failed: {}", ssl_errors());
        return nullptr;
    }

    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) != 1) {
        SPDLOG_ERROR("Failed to restrict TLS versions: {}", ssl_errors());
        return nullptr;
    }

    if (!opts.verify_peer) {
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
        return ctx;
    }

    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

    // CA: explicit path -> discovery -> OpenSSL default (fail-closed).
    std::optional<std::string> ca_path = opts.ca_bundle;
    if (!ca_path) {
        ca_path = Utils::Cert::discover_ca_bundle();
    }
    if (ca_path) {
        if (SSL_CTX_load_verify_locations(ctx.get(), ca_path->c_str(), nullptr) != 1) {
            SPDLOG_ERROR("Failed to load CA bundle from {}: {}", *ca_path, ssl_errors());
            return nullptr;
        }
    } else if (SSL_CTX_set_default_verify_paths(ctx.get()) != 1) {
        SPDLOG_ERROR("No CA bundle found and OpenSSL default paths failed: {}", ssl_errors());
        return nullptr;
    }

    return ctx;
}

[[nodiscard]] SslCtxPtr& shared_default_ctx() {
    static SslCtxPtr ctx = make_ssl_ctx(TlsOptions{});
    return ctx;
}

/// Frees a not-yet-published SSL session before closing the TCP connection.
class UnpublishedSession {
public:
    UnpublishedSession(detail::TcpConnection& connection, SslPtr& ssl) noexcept : connection_(connection), ssl_(ssl) {}

    UnpublishedSession(const UnpublishedSession&) = delete;
    UnpublishedSession& operator=(const UnpublishedSession&) = delete;

    void commit() noexcept { armed_ = false; }

    ~UnpublishedSession() {
        if (armed_) {
            ssl_.reset();
            connection_.close();
        }
    }

private:
    detail::TcpConnection& connection_;
    SslPtr& ssl_;
    bool armed_{true};
};

}  // namespace

void SslContextDeleter::operator()(SSL_CTX* ctx) const noexcept {
    SSL_CTX_free(ctx);
}

void SslDeleter::operator()(SSL* ssl) const noexcept {
    SSL_free(ssl);
}

TlsStream::TlsStream(std::string host, const std::uint16_t port, Options opts, TlsOptions tls_opts)
    : connection_(std::move(host), port, std::move(opts)), tls_opts_(std::move(tls_opts)),
      alpn_proto_(tls_opts_.alpn_proto.begin(), tls_opts_.alpn_proto.end()) {}

TlsStream::~TlsStream() {
    close();
}

SSL_CTX* TlsStream::ssl_ctx() const noexcept {
    if (custom_ctx_) {
        return custom_ctx_.get();
    }
    if (tls_opts_.verify_peer && !tls_opts_.ca_bundle) {
        return shared_default_ctx().get();
    }
    return nullptr;
}

std::expected<void, IoError> TlsStream::ensure_connected(const Utils::CancellationToken& token) {
    if (connection_.is_connected() && is_healthy()) {
        return {};
    }
    const auto deadline = std::chrono::steady_clock::now() + connection_.options().connect_timeout;
    return connect(deadline, token);
}

std::expected<void, IoError> TlsStream::connect(const std::chrono::steady_clock::time_point deadline,
                                                const Utils::CancellationToken& token) {
    using enum IoError;

    close();

    if (token.is_triggered()) {
        return std::unexpected(CANCELLED);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        return std::unexpected(TIMEOUT);
    }

    if (auto result = connection_.connect(deadline, token); !result) {
        return std::unexpected(result.error());
    }

    // Any failure after the TCP socket exists drops it. The SSL object is
    // published only after the handshake, and is freed before the fd.
    SSL_CTX* ctx = ssl_ctx();
    if (ctx == nullptr) {
        custom_ctx_ = make_ssl_ctx(tls_opts_);
        ctx = custom_ctx_.get();
        if (ctx == nullptr) {
            connection_.close();
            return std::unexpected(CONNECTION_FAILED);
        }
    }

    SslPtr ssl{SSL_new(ctx)};
    if (!ssl) {
        SPDLOG_ERROR("SSL_new failed: {}", ssl_errors());
        connection_.close();
        return std::unexpected(CONNECTION_FAILED);
    }
    UnpublishedSession rollback(connection_, ssl);

    SSL_set_fd(ssl.get(), connection_.socket().native_handle());

    const std::string& effective_hostname =
        tls_opts_.sni_hostname.has_value() ? *tls_opts_.sni_hostname : connection_.host();
    const bool is_ip = InetAddress::parse(effective_hostname).has_value();

    if (!is_ip) {
        // RFC 6066 §3: SNI MUST NOT contain an IP literal.
        SSL_set_tlsext_host_name(ssl.get(), effective_hostname.c_str());
    }

    auto* verify_param = SSL_get0_param(ssl.get());
    if (is_ip) {
        // Strip the IPv6 scope id ("fe80::1%eth0") — it is not part of the
        // address and X509_VERIFY_PARAM_set1_ip_asc rejects it.
        const auto pct = effective_hostname.find('%');
        const auto ip_literal = pct == std::string::npos ? effective_hostname : effective_hostname.substr(0, pct);
        if (X509_VERIFY_PARAM_set1_ip_asc(verify_param, ip_literal.c_str()) != 1) {
            SPDLOG_ERROR(R"(Failed to set IP verification for "{}": {})", effective_hostname, ssl_errors());
            return std::unexpected(CONNECTION_FAILED);
        }
    } else if (SSL_set1_host(ssl.get(), effective_hostname.c_str()) != 1) {
        SPDLOG_ERROR(R"(Failed to set hostname verification for "{}": {})", effective_hostname, ssl_errors());
        return std::unexpected(CONNECTION_FAILED);
    }

    if (!alpn_proto_.empty()) {
        if (alpn_proto_.size() > std::numeric_limits<unsigned int>::max() ||
            SSL_set_alpn_protos(ssl.get(), alpn_proto_.data(), static_cast<unsigned int>(alpn_proto_.size())) != 0) {
            SPDLOG_ERROR("SSL_set_alpn_protos failed: {}", ssl_errors());
            return std::unexpected(CONNECTION_FAILED);
        }
    }

    if (auto result = handshake(ssl.get(), deadline, token); !result) {
        return result;
    }

    ssl_ = std::move(ssl);
    rollback.commit();
    SPDLOG_DEBUG(R"(TLS connection established to "{}:{}" ({}))", connection_.host(), connection_.port(),
                 SSL_get_version(ssl_.get()));
    return {};
}

std::expected<void, IoError> TlsStream::handshake(SSL* ssl, const std::chrono::steady_clock::time_point deadline,
                                                  const Utils::CancellationToken& token) {
    using enum IoError;

    for (;;) {
        if (token.is_triggered()) {
            return std::unexpected(CANCELLED);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(TIMEOUT);
        }

        const int rc = SSL_connect(ssl);
        if (rc == 1) {
            return {};
        }

        const int err = SSL_get_error(ssl, rc);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            SPDLOG_ERROR(R"(TLS handshake failed for "{}:{}": {})", connection_.host(), connection_.port(),
                         ssl_errors());
            return std::unexpected(CONNECTION_FAILED);
        }

        auto waited = detail::wait_for_tls_direction(err, connection_.socket(), deadline, token);
        if (!waited) {
            return std::unexpected(waited.error());
        }
    }
}

bool TlsStream::is_healthy() const noexcept {
    if (ssl_ == nullptr || !connection_.is_connected()) {
        return false;
    }
    // Buffered application data means the connection is alive even when
    // the raw socket shows nothing readable.
    if (SSL_pending(ssl_.get()) > 0) {
        return true;
    }
    return connection_.is_healthy();
}

std::expected<size_t, IoError> TlsStream::read_some(const std::span<std::uint8_t> buf,
                                                    const Utils::CancellationToken& token) {
    if (buf.empty()) {
        return 0;
    }
    if (ssl_ == nullptr) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    const auto deadline = std::chrono::steady_clock::now() + connection_.options().read_timeout;
    return read_once(buf, deadline, token);
}

std::expected<size_t, IoError> TlsStream::read_once(const std::span<std::uint8_t> buf,
                                                    const std::chrono::steady_clock::time_point deadline,
                                                    const Utils::CancellationToken& token) {
    return detail::read_ssl(ssl_.get(), connection_.socket(), buf, deadline, token);
}

std::expected<void, IoError> TlsStream::read_exact(const std::span<std::uint8_t> buf,
                                                   const Utils::CancellationToken& token) {
    if (buf.empty()) {
        return {};
    }
    if (ssl_ == nullptr) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }

    const auto deadline = std::chrono::steady_clock::now() + connection_.options().read_timeout;
    auto remaining = buf;
    while (!remaining.empty()) {
        auto n = read_once(remaining, deadline, token);
        if (!n) {
            return std::unexpected(n.error());
        }
        remaining = remaining.subspan(*n);
    }
    return {};
}

std::expected<void, IoError> TlsStream::send_all(const std::span<const std::uint8_t> data,
                                                 const Utils::CancellationToken& token) {
    if (data.empty()) {
        return {};
    }
    if (ssl_ == nullptr) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }

    const auto deadline = std::chrono::steady_clock::now() + connection_.options().write_timeout;
    return detail::write_ssl(ssl_.get(), connection_.socket(), data, deadline, token);
}

void TlsStream::close() noexcept {
    // SSL_free before the fd it borrows is closed.
    ssl_.reset();
    connection_.close();
}

}  // namespace Transport
