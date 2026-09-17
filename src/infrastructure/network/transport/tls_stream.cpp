//
// TlsStream — self-managing TLS byte stream (Transport).
//
#include "infrastructure/network/transport/tls_stream.h"

#include <netinet/in.h>
#include <poll.h>

#include <array>
#include <chrono>
#include <optional>
#include <utility>

#include <openssl/err.h>

#include <spdlog/spdlog.h>

#include "domain/network/inet_address.h"
#include "support/util/cancellation_token.hpp"
#include "infrastructure/network/tls/cert_util.h"

#include "support/fmt.hpp"

namespace Transport {

namespace {

    /// Format the OpenSSL error stack for logging.
    [[nodiscard]] std::string ssl_errors() {
        std::vector<std::string> errors;
        unsigned long err;
        while ((err = ERR_get_error()) != 0) {
            std::array<char, 256> buf{};
            ERR_error_string_n(err, buf.data(), buf.size());
            errors.emplace_back(buf.data());
        }
        return fmt::format("{}", fmt::join(errors, "; "));
    }

    /// Build an SSL_CTX for the given options.
    ///
    /// Shared default (verify-on, auto-discovered CA) is used when the
    /// options match it; otherwise a per-instance ctx is built (custom CA
    /// bundle or verification disabled).
    [[nodiscard]] SslCtxPtr make_ssl_ctx(const TlsOptions &opts) {
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

    [[nodiscard]] SslCtxPtr &shared_default_ctx() {
        static SslCtxPtr ctx = make_ssl_ctx(TlsOptions{});
        return ctx;
    }

} // namespace

void SslContextDeleter::operator()(SSL_CTX *ctx) const noexcept {
    SSL_CTX_free(ctx);
}

TlsStream::TlsStream(std::string host, const std::uint16_t port, Options opts, TlsOptions tls_opts,
                     Utils::CancellationToken token)
    : socket_(std::move(host), port, opts, std::move(token)),
      opts_(std::move(opts)),
      tls_opts_(std::move(tls_opts)),
      alpn_proto_(tls_opts_.alpn_proto.begin(), tls_opts_.alpn_proto.end()) {
}

TlsStream::~TlsStream() {
    close();
}

SSL_CTX *TlsStream::ssl_ctx() const noexcept {
    if (custom_ctx_) {
        return custom_ctx_.get();
    }
    if (tls_opts_.verify_peer && !tls_opts_.ca_bundle) {
        return shared_default_ctx().get();
    }
    return nullptr;
}

std::expected<void, IoError> TlsStream::ensure_connected() {
    if (socket_.is_connected() && is_healthy()) {
        return {};
    }
    return connect(std::chrono::steady_clock::now() + opts_.connect_timeout);
}

std::expected<void, IoError> TlsStream::connect(const std::chrono::steady_clock::time_point deadline) {
    using enum IoError;

    close();

    if (auto result = socket_.connect(); !result) {
        return std::unexpected(result.error());
    }

    // Resolve the SSL_CTX (shared default or per-instance custom).
    SSL_CTX *ctx = ssl_ctx();
    if (ctx == nullptr) {
        custom_ctx_ = make_ssl_ctx(tls_opts_);
        ctx = custom_ctx_.get();
        if (ctx == nullptr) {
            return std::unexpected(CONNECTION_FAILED);
        }
    }

    ssl_ = SSL_new(ctx);
    if (ssl_ == nullptr) {
        SPDLOG_ERROR("SSL_new failed: {}", ssl_errors());
        return std::unexpected(CONNECTION_FAILED);
    }
    SSL_set_fd(ssl_, socket_.fd());

    const std::string &effective_hostname = tls_opts_.sni_hostname.has_value() ? *tls_opts_.sni_hostname : socket_.host();
    const bool is_ip = InetAddress::parse(effective_hostname).has_value();

    if (!is_ip) {
        // RFC 6066 §3: SNI MUST NOT contain an IP literal.
        SSL_set_tlsext_host_name(ssl_, effective_hostname.c_str());
    }

    // Bind peer identity to the connection target.
    auto *verify_param = SSL_get0_param(ssl_);
    if (is_ip) {
        // Strip the IPv6 scope id ("fe80::1%eth0") — it is not part of the
        // address and X509_VERIFY_PARAM_set1_ip_asc rejects it.
        const auto pct = effective_hostname.find('%');
        const auto ip_literal = pct == std::string::npos
                                    ? effective_hostname
                                    : effective_hostname.substr(0, pct);
        if (X509_VERIFY_PARAM_set1_ip_asc(verify_param, ip_literal.c_str()) != 1) {
            SPDLOG_ERROR(R"(Failed to set IP verification for "{}": {})", effective_hostname, ssl_errors());
            return std::unexpected(CONNECTION_FAILED);
        }
    } else if (SSL_set1_host(ssl_, effective_hostname.c_str()) != 1) {
        SPDLOG_ERROR(R"(Failed to set hostname verification for "{}": {})", effective_hostname, ssl_errors());
        return std::unexpected(CONNECTION_FAILED);
    }

    if (!alpn_proto_.empty()) {
        if (SSL_set_alpn_protos(ssl_, alpn_proto_.data(), static_cast<unsigned>(alpn_proto_.size())) != 0) {
            SPDLOG_ERROR("SSL_set_alpn_protos failed: {}", ssl_errors());
            return std::unexpected(CONNECTION_FAILED);
        }
    }

    auto result = handshake(deadline);
    if (result) {
        SPDLOG_DEBUG(R"(TLS connection established to "{}:{}" ({}))", socket_.host(), socket_.port(),
                     SSL_get_version(ssl_));
    }
    return result;
}

std::expected<void, IoError> TlsStream::handshake(const std::chrono::steady_clock::time_point deadline) {
    using enum IoError;

    for (;;) {
        const int rc = SSL_connect(ssl_);
        if (rc == 1) {
            return {};
        }

        const int err = SSL_get_error(ssl_, rc);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            SPDLOG_ERROR(R"(TLS handshake failed for "{}:{}": {})", socket_.host(), socket_.port(),
                         ssl_errors());
            return std::unexpected(CONNECTION_FAILED);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::unexpected(TIMEOUT);
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (auto ready = socket_.poll(err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, remaining); !ready) {
            return std::unexpected(ready.error());
        }
    }
}

bool TlsStream::is_healthy() const noexcept {
    if (ssl_ == nullptr || !socket_.is_connected()) {
        return false;
    }
    // Buffered application data means the connection is alive even when
    // the raw socket shows nothing readable.
    if (SSL_pending(ssl_) > 0) {
        return true;
    }
    return socket_.is_healthy();
}

std::expected<size_t, IoError> TlsStream::read_some(const std::span<std::uint8_t> buf) {
    if (ssl_ == nullptr) {
        return std::unexpected(IoError::CONNECTION_FAILED);
    }
    if (buf.empty()) {
        return 0;
    }
    return read_once(buf);
}

std::expected<size_t, IoError> TlsStream::read_once(const std::span<std::uint8_t> buf) {
    using enum IoError;

    for (;;) {
        // Skip the poll when OpenSSL already holds decrypted data.
        if (SSL_pending(ssl_) == 0) {
            if (auto ready = socket_.poll(POLLIN, opts_.read_timeout); !ready) {
                return std::unexpected(ready.error());
            }
        }

        const int rc = SSL_read(ssl_, buf.data(), static_cast<int>(buf.size()));
        if (rc > 0) {
            return static_cast<size_t>(rc);
        }

        const int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            continue; // poll again with the respective readiness direction
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            // close_notify: the peer closed the connection cleanly.
            return std::unexpected(CONNECTION_FAILED);
        }
        SPDLOG_DEBUG("TLS read failed: {}", ssl_errors());
        return std::unexpected(CONNECTION_FAILED);
    }
}

std::expected<void, IoError> TlsStream::read_exact(const std::span<std::uint8_t> buf) {
    auto remaining = buf;
    while (!remaining.empty()) {
        auto n = read_once(remaining);
        if (!n) {
            return std::unexpected(n.error());
        }
        remaining = remaining.subspan(*n);
    }
    return {};
}

std::expected<void, IoError> TlsStream::send_all(const std::span<const std::uint8_t> data) {
    using enum IoError;

    if (ssl_ == nullptr) {
        return std::unexpected(CONNECTION_FAILED);
    }

    auto remaining = data;
    while (!remaining.empty()) {
        const int rc = SSL_write(ssl_, remaining.data(), static_cast<int>(remaining.size()));
        if (rc > 0) {
            remaining = remaining.subspan(static_cast<size_t>(rc));
            continue;
        }

        const int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            const auto timeout = err == SSL_ERROR_WANT_READ ? opts_.read_timeout : opts_.write_timeout;
            if (auto ready = socket_.poll(err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, timeout); !ready) {
                return std::unexpected(ready.error());
            }
            continue;
        }
        SPDLOG_DEBUG("TLS write failed: {}", ssl_errors());
        return std::unexpected(CONNECTION_FAILED);
    }
    return {};
}

void TlsStream::close() noexcept {
    if (ssl_ != nullptr) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    socket_.close();
}

} // namespace Transport
