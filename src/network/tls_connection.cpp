//
// Created by Kotarou on 2026/7/10.
//

#include "tls_connection.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <source_location>
#include <string>

#include "exception/tls.h"
#include "network/inet_address.h"
#include "util/cert_util.hpp"
#include "util/validation.hpp"

#include "fmt.hpp"
#include <openssl/err.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

// ===========================================================================
//  RAII deleter implementations
// ===========================================================================

void SSLContextDeleter::operator()(SSL_CTX *ctx) const noexcept {
    SSL_CTX_free(ctx);
}

void BIODeleter::operator()(BIO *bio) const noexcept {
    BIO_free_all(bio);
}

// ===========================================================================
//  Format the OpenSSL error stack and throw TlsException
// ===========================================================================

namespace {
    void throw_ssl_error(std::string_view context, const std::source_location &loc = std::source_location::current()) {
        std::string msg;
        msg.reserve(256);
        msg += context;
        msg += " [";

        unsigned long err;
        int first = 1;
        while ((err = ERR_get_error()) != 0) {
            if (!first)
                msg += "; ";
            first = 0;

            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            msg += buf;
        }
        msg += "]";

        msg += fmt::format(" (at {}:{}:{})", loc.file_name(), loc.line(), loc.column());

        throw TlsException(msg);
    }
} // anonymous namespace

// ===========================================================================
//  Construction / destruction
// ===========================================================================

TlsConnection::TlsConnection(std::string server, std::uint16_t port, std::chrono::milliseconds connect_timeout,
                             std::optional<std::string> sni_hostname, std::span<const std::uint8_t> alpn_proto,
                             ContextFactory context_factory)
    : server_(std::move(server)), port_(port), connect_timeout_(connect_timeout),
      sni_hostname_(std::move(sni_hostname)), alpn_proto_(alpn_proto.begin(), alpn_proto.end()),
      context_factory_(std::move(context_factory)) {
    // Validate server address eagerly so the caller gets a clear error.
    if (!InetAddress::parse(server_).has_value() && !Utils::is_valid_domain(server_)) {
        throw TlsException(fmt::format(R"(Invalid server address: "{}" (not a valid IP or domain name))", server_));
    }
}

TlsConnection::~TlsConnection() = default;

TlsConnection::TlsConnection(TlsConnection &&) noexcept = default;

TlsConnection &TlsConnection::operator=(TlsConnection &&) noexcept = default;

// ===========================================================================
//  Lifecycle
// ===========================================================================

void TlsConnection::connect() {
    close();

    // Resolve the SSL_CTX: custom factory or shared default.
    SSL_CTX *ctx;
    if (context_factory_) {
        if (!custom_ctx_) {
            custom_ctx_ = context_factory_();
        }
        ctx = custom_ctx_.get();
    } else {
        ctx = get_shared_ssl_ctx();
    }

    BioPtr bio(BIO_new_ssl_connect(ctx));
    if (!bio) {
        throw_ssl_error("BIO_new_ssl_connect");
    }

    BIO_get_ssl(bio.get(), &ssl_);
    if (ssl_) {
        // SNI and certificate hostname verification — always the same value.
        // sni_hostname_ overrides server_ when set, otherwise server_ is used
        // for both purposes (even when it is an IP address).
        const std::string &effective_hostname = sni_hostname_.has_value() ? *sni_hostname_ : server_;
        SSL_set_tlsext_host_name(ssl_, effective_hostname.c_str());
        SSL_set1_host(ssl_, effective_hostname.c_str());

        if (!alpn_proto_.empty()) {
            if (SSL_set_alpn_protos(ssl_, alpn_proto_.data(), static_cast<unsigned>(alpn_proto_.size())) != 0) {
                throw_ssl_error("SSL_set_alpn_protos");
            }
        }
    }

    const auto target = fmt::format("{}:{}", server_, port_);
    if (BIO_set_conn_hostname(bio.get(), target.c_str()) != 1) {
        throw_ssl_error(fmt::format("BIO_set_conn_hostname({})", target));
    }

    // Non-blocking connect with timeout.
    BIO_set_nbio(bio.get(), 1);

    const auto deadline = std::chrono::steady_clock::now() + connect_timeout_;

    for (;;) {
        const auto ret = BIO_do_connect(bio.get());
        if (ret == 1)
            break;

        if (!BIO_should_retry(bio.get())) {
            throw_ssl_error(fmt::format(R"(TLS connect/handshake failed for "{}")", target));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw TlsException(
                fmt::format(R"(TLS connection timeout ({}ms) for "{}")", connect_timeout_.count(), target));
        }

        const auto fd_raw = BIO_get_fd(bio.get(), nullptr);
        const int fd = static_cast<int>(fd_raw);
        if (fd == -1) {
            throw TlsException(fmt::format(R"(TLS failed to get socket fd for "{}")", target));
        }

        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

        pollfd pfd{
            .fd = fd,
            .events = static_cast<int16_t>(BIO_should_read(bio.get()) ? POLLIN : POLLOUT),
            .revents = 0,
        };

        const int poll_ret = poll(&pfd, 1, static_cast<int>(remaining_ms.count()));
        if (poll_ret <= 0) {
            if (poll_ret == 0) {
                throw TlsException(
                    fmt::format(R"(TLS connection timeout ({}ms) for "{}")", connect_timeout_.count(), target));
            }
            if (errno == EINTR) {
                continue;
            }
            throw TlsException(
                fmt::format(R"(TLS poll() failed for "{}": {})", target, std::strerror(errno)));
        }
    }

    SSL *connected_ssl = nullptr;
    BIO_get_ssl(bio.get(), &connected_ssl);
    SPDLOG_TRACE(R"(TLS connection established to "{}" (tls_version: {}))", target,
                 connected_ssl ? SSL_get_version(connected_ssl) : "?");

    bio_ = std::move(bio);
}

void TlsConnection::close() noexcept {
    bio_.reset();
    ssl_ = nullptr;
}

bool TlsConnection::is_healthy() const noexcept {
    if (!bio_ || !ssl_)
        return false;

    // Pending TLS application data → connection is definitely alive.
    if (SSL_pending(ssl_) > 0)
        return true;

    const auto fd_raw = BIO_get_fd(bio_.get(), nullptr);
    if (fd_raw < 0)
        return false;

    const int fd = static_cast<int>(fd_raw);
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    const int ret = poll(&pfd, 1, 0);
    if (ret <= 0)
        return true; // No data pending, connection is alive.

    // POLLIN — could be data or EOF (peer closed).  Peek to distinguish.
    char c;
    const auto n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n < 0) {
        // EAGAIN / EWOULDBLOCK mean no data yet (spurious POLLIN);
        // any other error (ECONNRESET, etc.) means the connection is dead.
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    return n > 0; // n == 0 means EOF.
}

// ===========================================================================
//  I/O
// ===========================================================================

TlsConnection::IoStatus TlsConnection::send_all(std::span<const std::uint8_t> data, int cancel_fd) {
    if (!bio_)
        return IoStatus::ERROR;

    while (!data.empty()) {
        // Determine poll direction from OpenSSL's retry flags — a TLS
        // renegotiation or key update may require reading during a write.
        auto event = PollEvent::WRITE;
        if (BIO_should_retry(bio_.get())) {
            event = BIO_should_read(bio_.get()) ? PollEvent::READ : PollEvent::WRITE;
        }

        const auto status = poll_bio(bio_.get(), event, cancel_fd);
        if (status != IoStatus::OK)
            return status;

        BIO_clear_retry_flags(bio_.get());
        const int rc = BIO_write(bio_.get(), data.data(), static_cast<int>(data.size()));
        if (rc > 0) {
            data = data.subspan(static_cast<size_t>(rc));
        } else if (!BIO_should_retry(bio_.get())) {
            return IoStatus::ERROR;
        }
    }
    return IoStatus::OK;
}

TlsConnection::IoStatus TlsConnection::read_exact(std::span<std::uint8_t> buf, int cancel_fd) {
    if (!bio_)
        return IoStatus::ERROR;

    while (!buf.empty()) {
        // Determine poll direction from OpenSSL's retry flags — a TLS
        // renegotiation or key update may require writing during a read.
        auto event = PollEvent::READ;
        if (BIO_should_retry(bio_.get())) {
            event = BIO_should_write(bio_.get()) ? PollEvent::WRITE : PollEvent::READ;
        }

        const auto status = poll_bio(bio_.get(), event, cancel_fd);
        if (status != IoStatus::OK)
            return status;

        BIO_clear_retry_flags(bio_.get());
        const int rc = BIO_read(bio_.get(), buf.data(), static_cast<int>(buf.size()));
        if (rc > 0) {
            buf = buf.subspan(static_cast<size_t>(rc));
        } else if (!BIO_should_retry(bio_.get())) {
            return IoStatus::ERROR;
        }
    }
    return IoStatus::OK;
}

TlsConnection::IoStatus TlsConnection::shutdown() {
    if (!ssl_)
        return IoStatus::ERROR;

    for (int attempt = 0;; ++attempt) {
        if (attempt > 0) {
            // On retry, determine poll direction from retry flags.
            auto event = PollEvent::READ;
            if (BIO_should_retry(bio_.get())) {
                event = BIO_should_read(bio_.get()) ? PollEvent::READ : PollEvent::WRITE;
            }

            const auto status = poll_bio(bio_.get(), event, -1);
            if (status != IoStatus::OK)
                return status;
        }

        BIO_clear_retry_flags(bio_.get());
        const int ret = SSL_shutdown(ssl_);
        if (ret == 1 || ret == 0)
            return IoStatus::OK;

        const int err = SSL_get_error(ssl_, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
            return IoStatus::ERROR;
    }
}

std::expected<size_t, TlsConnection::IoStatus> TlsConnection::read_some(std::span<std::uint8_t> buf, int cancel_fd) {
    if (!bio_)
        return std::unexpected(IoStatus::ERROR);

    for (;;) {
        auto event = PollEvent::READ;
        if (BIO_should_retry(bio_.get())) {
            event = BIO_should_write(bio_.get()) ? PollEvent::WRITE : PollEvent::READ;
        }

        const auto status = poll_bio(bio_.get(), event, cancel_fd);
        if (status != IoStatus::OK)
            return std::unexpected(status);

        BIO_clear_retry_flags(bio_.get());
        const int rc = BIO_read(bio_.get(), buf.data(), static_cast<int>(buf.size()));
        if (rc > 0)
            return static_cast<size_t>(rc);

        if (!BIO_should_retry(bio_.get()))
            return std::unexpected(IoStatus::ERROR);
    }
}

std::string TlsConnection::negotiated_alpn() const noexcept {
    if (!ssl_)
        return {};

    const unsigned char *data = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(ssl_, &data, &len);
    return {reinterpret_cast<const char *>(data), len};
}

void TlsConnection::set_sni_hostname(std::string hostname) {
    sni_hostname_ = std::move(hostname);
}

// ===========================================================================
//  Internals
// ===========================================================================

TlsConnection::IoStatus TlsConnection::poll_bio(BIO *bio, PollEvent event, int cancel_fd) {
    const int fd = static_cast<int>(BIO_get_fd(bio, nullptr));
    if (fd < 0)
        return IoStatus::ERROR;

    pollfd fds[2] = {};
    fds[0].fd = fd;
    fds[0].events = static_cast<int16_t>(event == PollEvent::READ ? POLLIN : POLLOUT);

    int nfds = 1;
    if (cancel_fd >= 0) {
        fds[1].fd = cancel_fd;
        fds[1].events = POLLIN;
        nfds = 2;
    }

    int ret;
    do {
        ret = ::poll(fds, static_cast<nfds_t>(nfds), static_cast<int>(io_timeout_ms_.count()));
    } while (ret < 0 && errno == EINTR);
    if (ret == 0)
        return IoStatus::TIMEOUT;
    if (ret < 0)
        return IoStatus::ERROR;

    if (nfds == 2 && (fds[1].revents & POLLIN)) {
        return IoStatus::CANCELLED;
    }

    return (fds[0].revents & fds[0].events) != 0 ? IoStatus::OK : IoStatus::ERROR;
}

SslCtxPtr TlsConnection::create_default_ssl_ctx() {
    SslCtxPtr ctx(SSL_CTX_new(TLS_client_method()));
    if (!ctx)
        throw_ssl_error("SSL_CTX_new");

    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) != 1)
        throw_ssl_error("SSL_CTX_set_min_proto_version");
    if (SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) != 1)
        throw_ssl_error("SSL_CTX_set_max_proto_version");

    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

    if (SSL_CTX_set_default_verify_paths(ctx.get()) != 1) {
        SPDLOG_DEBUG("SSL_CTX_set_default_verify_paths failed, falling back to cert_util");
        if (!Utils::Cert::get_system_ca_path().and_then(
            [raw = ctx.get()](const auto &path) -> std::optional<std::string> {
                if (SSL_CTX_load_verify_locations(raw, path.c_str(), nullptr) != 1) {
                    return std::nullopt;
                }
                SPDLOG_DEBUG("Loaded CA bundle from {}", path);
                return path;
            })) {
            throw_ssl_error("SSL_CTX_set_default_verify_paths and cert_util both failed");
        }
    }

    return ctx;
}

SSL_CTX *TlsConnection::get_shared_ssl_ctx() {
    static const SslCtxPtr ctx = create_default_ssl_ctx();
    return ctx.get();
}
