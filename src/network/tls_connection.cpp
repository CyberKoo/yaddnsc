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
#include <netinet/in.h>
#include <netinet/tcp.h>
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
//  Format the OpenSSL error stack and log + return IoStatus::ERROR
// ===========================================================================

namespace {
    TlsConnection::IoStatus log_ssl_error(std::string_view context) {
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

        SPDLOG_ERROR("TLS error: {}", msg);
        return TlsConnection::IoStatus::ERROR;
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

TlsConnection::TlsConnection(TlsConnection &&other) noexcept
    : server_(std::move(other.server_)),
      port_(other.port_),
      connect_timeout_(other.connect_timeout_),
      sni_hostname_(std::move(other.sni_hostname_)),
      alpn_proto_(std::move(other.alpn_proto_)),
      context_factory_(std::move(other.context_factory_)),
      io_timeout_ms_(other.io_timeout_ms_),
      custom_ctx_(std::move(other.custom_ctx_)),
      bio_(std::move(other.bio_)),
      ssl_(std::exchange(other.ssl_, nullptr)) {}

TlsConnection &TlsConnection::operator=(TlsConnection &&other) noexcept {
    if (this == &other)
        return *this;
    server_ = std::move(other.server_);
    port_ = other.port_;
    connect_timeout_ = other.connect_timeout_;
    sni_hostname_ = std::move(other.sni_hostname_);
    alpn_proto_ = std::move(other.alpn_proto_);
    context_factory_ = std::move(other.context_factory_);
    io_timeout_ms_ = other.io_timeout_ms_;
    custom_ctx_ = std::move(other.custom_ctx_);
    bio_ = std::move(other.bio_);
    ssl_ = std::exchange(other.ssl_, nullptr);
    return *this;
}

// ===========================================================================
//  Lifecycle
// ===========================================================================

std::expected<void, TlsConnection::IoStatus> TlsConnection::connect() {
    close();

    // Resolve the SSL_CTX: custom factory or shared default.
    SSL_CTX *ctx;
    if (context_factory_) {
        if (!custom_ctx_) {
            custom_ctx_ = context_factory_();
        }
        ctx = custom_ctx_.get();
        if (!ctx) {
            return std::unexpected(IoStatus::ERROR);
        }
    } else {
        ctx = get_shared_ssl_ctx();
        if (!ctx) {
            return std::unexpected(IoStatus::ERROR);
        }
    }

    BioPtr bio(BIO_new_ssl_connect(ctx));
    if (!bio) {
        return std::unexpected(log_ssl_error("BIO_new_ssl_connect"));
    }

    const auto target = fmt::format("{}:{}", server_, port_);

    // ssl_ borrows from `bio` below.  If any error path is taken after this
    // point, ssl_ must be reset to nullptr before returning, otherwise it
    // becomes a dangling pointer when the local `bio` is destroyed.
    BIO_get_ssl(bio.get(), &ssl_);
    if (ssl_) {
        const std::string &effective_hostname = sni_hostname_.has_value() ? *sni_hostname_ : server_;
        SSL_set_tlsext_host_name(ssl_, effective_hostname.c_str());
        SSL_set1_host(ssl_, effective_hostname.c_str());

        if (!alpn_proto_.empty()) {
            if (SSL_set_alpn_protos(ssl_, alpn_proto_.data(), static_cast<unsigned>(alpn_proto_.size())) != 0) {
                ssl_ = nullptr;
                return std::unexpected(log_ssl_error("SSL_set_alpn_protos"));
            }
        }
    }

    if (BIO_set_conn_hostname(bio.get(), target.c_str()) != 1) {
        ssl_ = nullptr;
        return std::unexpected(log_ssl_error(fmt::format("BIO_set_conn_hostname({})", target)));
    }

    // Non-blocking connect with timeout.
    BIO_set_nbio(bio.get(), 1);

    const auto deadline = std::chrono::steady_clock::now() + connect_timeout_;

    for (;;) {
        // Clear retry flags before each attempt so that BIO_should_retry
        // reflects the result of this call, not a stale value from a prior
        // iteration.  (OpenSSL internally clears flags for ssl BIO in
        // non-blocking mode, but being explicit here is defensive.)
        BIO_clear_retry_flags(bio.get());

        const auto ret = BIO_do_connect(bio.get());
        if (ret == 1)
            break;

        if (!BIO_should_retry(bio.get())) {
            ssl_ = nullptr;
            return std::unexpected(log_ssl_error(fmt::format(R"(TLS connect/handshake failed for "{}")", target)));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ssl_ = nullptr;
            return std::unexpected(IoStatus::TIMEOUT);
        }

        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

        const auto pstatus = poll_bio(bio.get(), POLLOUT, -1, remaining_ms);
        if (pstatus == IoStatus::TIMEOUT) {
            ssl_ = nullptr;
            return std::unexpected(IoStatus::TIMEOUT);
        }
        if (pstatus != IoStatus::OK) {
            ssl_ = nullptr;
            return std::unexpected(IoStatus::ERROR);
        }
    }

    // Enable TCP_NODELAY to disable Nagle's algorithm — TLS handshakes and
    // DNS queries are latency-sensitive; Nagle's algorithm can delay small
    // packets, increasing response time.
    const auto fd_raw = BIO_get_fd(bio.get(), nullptr);
    if (fd_raw >= 0) {
        const int fd = static_cast<int>(fd_raw);
        const int one = 1;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
            SPDLOG_WARN(R"(Failed to set TCP_NODELAY on TLS socket to "{}": {})", target, std::strerror(errno));
        }
    }

    SSL *connected_ssl = nullptr;
    BIO_get_ssl(bio.get(), &connected_ssl);
    SPDLOG_TRACE(R"(TLS connection established to "{}" (tls_version: {}))", target,
                 connected_ssl ? SSL_get_version(connected_ssl) : "?");

    bio_ = std::move(bio);
    return {};
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

std::expected<void, TlsConnection::IoStatus> TlsConnection::send_all(std::span<const std::uint8_t> data, int cancel_fd) {
    if (!bio_)
        return std::unexpected(IoStatus::ERROR);

    while (!data.empty()) {
        const auto status = poll_bio(bio_.get(), POLLOUT, cancel_fd, io_timeout_ms_);
        if (status != IoStatus::OK)
            return std::unexpected(status);

        BIO_clear_retry_flags(bio_.get());
        const int rc = BIO_write(bio_.get(), data.data(), static_cast<int>(data.size()));
        if (rc > 0) {
            data = data.subspan(static_cast<size_t>(rc));
        } else if (!BIO_should_retry(bio_.get())) {
            return std::unexpected(IoStatus::ERROR);
        }
    }
    return {};
}

std::expected<void, TlsConnection::IoStatus> TlsConnection::read_exact(std::span<std::uint8_t> buf, int cancel_fd) {
    if (!bio_)
        return std::unexpected(IoStatus::ERROR);

    while (!buf.empty()) {
        // If OpenSSL already has decrypted data buffered in its internal
        // read buffer (e.g. from a previous SSL_read that consumed a large
        // TLS record), skip the poll — the data is already available at the
        // SSL layer and the raw socket may have nothing new to read.
        if (!ssl_ || SSL_pending(ssl_) == 0) {
            const auto status = poll_bio(bio_.get(), POLLIN, cancel_fd, io_timeout_ms_);
            if (status != IoStatus::OK)
                return std::unexpected(status);
        }

        BIO_clear_retry_flags(bio_.get());
        const int rc = BIO_read(bio_.get(), buf.data(), static_cast<int>(buf.size()));
        if (rc > 0) {
            buf = buf.subspan(static_cast<size_t>(rc));
        } else if (!BIO_should_retry(bio_.get())) {
            return std::unexpected(IoStatus::ERROR);
        }
    }
    return {};
}

std::expected<void, TlsConnection::IoStatus> TlsConnection::shutdown() {
    if (!ssl_)
        return std::unexpected(IoStatus::ERROR);

    for (int attempt = 0;; ++attempt) {
        if (attempt > 0) {
            const auto status = poll_bio(bio_.get(), POLLIN, -1, io_timeout_ms_);
            if (status != IoStatus::OK)
                return std::unexpected(status);
        }

        BIO_clear_retry_flags(bio_.get());
        const int ret = SSL_shutdown(ssl_);
        if (ret == 1 || ret == 0)
            return {};

        const int err = SSL_get_error(ssl_, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
            return std::unexpected(IoStatus::ERROR);
    }
}

std::expected<size_t, TlsConnection::IoStatus> TlsConnection::read_some(std::span<std::uint8_t> buf, int cancel_fd) {
    if (!bio_)
        return std::unexpected(IoStatus::ERROR);

    for (;;) {
        // Same as read_exact: skip poll when buffered data already exists.
        if (!ssl_ || SSL_pending(ssl_) == 0) {
            const auto status = poll_bio(bio_.get(), POLLIN, cancel_fd, io_timeout_ms_);
            if (status != IoStatus::OK)
                return std::unexpected(status);
        }

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

TlsConnection::IoStatus TlsConnection::poll_bio(BIO *bio, short default_events, int cancel_fd,
                                                  std::chrono::milliseconds timeout) {
    const int fd = static_cast<int>(BIO_get_fd(bio, nullptr));
    if (fd < 0)
        return IoStatus::ERROR;

    // Combine BIO's need flags with the caller's default:
    // - If OpenSSL has pending handshake/renegotiation work, it tells us
    //   which direction(s) it needs via BIO_should_read / BIO_should_write.
    // - On the first call (no retry flags set), these return false, so we
    //   fall back to the caller's default (POLLIN for reads, POLLOUT for writes).
    short events = 0;
    if (BIO_should_read(bio))
        events |= POLLIN;
    if (BIO_should_write(bio))
        events |= POLLOUT;
    if (events == 0)
        events = default_events;

    pollfd fds[2] = {};
    fds[0].fd = fd;
    fds[0].events = events;

    int nfds = 1;
    if (cancel_fd >= 0) {
        fds[1].fd = cancel_fd;
        fds[1].events = POLLIN;
        nfds = 2;
    }

    int ret;
    do {
        ret = ::poll(fds, static_cast<nfds_t>(nfds), static_cast<int>(timeout.count()));
    } while (ret < 0 && errno == EINTR);
    if (ret == 0)
        return IoStatus::TIMEOUT;
    if (ret < 0)
        return IoStatus::ERROR;

    if (nfds == 2 && (fds[1].revents & POLLIN)) {
        return IoStatus::CANCELLED;
    }

    // Requested events ready → OK (even if POLLHUP is also set, data is still
    // readable/writable). Let the subsequent BIO_read/BIO_write handle any
    // underlying connection issues, which gives us a more accurate error.
    if (fds[0].revents & fds[0].events) {
        return IoStatus::OK;
    }

    // Pure error/hangup with no data ready → ERROR.
    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return IoStatus::ERROR;
    }

    // poll() returned but no matching event — should not normally happen.
    return IoStatus::ERROR;
}

SslCtxPtr TlsConnection::create_default_ssl_ctx() {
    SslCtxPtr ctx(SSL_CTX_new(TLS_client_method()));
    if (!ctx) {
        log_ssl_error("SSL_CTX_new");
        return nullptr;
    }

    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) != 1) {
        log_ssl_error("SSL_CTX_set_min_proto_version");
        return nullptr;
    }
    if (SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) != 1) {
        log_ssl_error("SSL_CTX_set_max_proto_version");
        return nullptr;
    }

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
            log_ssl_error("SSL_CTX_set_default_verify_paths and cert_util both failed");
            return nullptr;
        }
    }

    return ctx;
}

SSL_CTX *TlsConnection::get_shared_ssl_ctx() {
    static const SslCtxPtr ctx = create_default_ssl_ctx();
    return ctx.get();
}
