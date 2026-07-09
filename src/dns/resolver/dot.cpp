//
// Created by Kotarou on 2026/6/29.
//

#include "dot.h"

#include <chrono>
#include <mutex>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dns/resolver_registry.h"
#include "dns/util.hpp"
#include "dns/validator.h"
#include "dns/wire/query.h"
#include "exception/dns_lookup.h"
#include "network/inet_address.h"
#include "util/bytes.hpp"
#include "util/cert_util.hpp"
#include "util/validation.hpp"

#include "dns_error.h"
#include "uri.h"

#include "fmt.hpp"
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

namespace {
    using namespace std::chrono_literals;

    // ── I/O timeout for poll() on the established TLS connection. ──
    constexpr int IO_TIMEOUT_MS = 5000;

    // ── poll() helper for BIO I/O ──

    enum class PollEvent {
        READ,
        WRITE
    };

    // ── RAII wrappers for OpenSSL resources ──

    struct SSLContextDeleter {
        void operator()(SSL_CTX *ctx) const noexcept {
            SSL_CTX_free(ctx);
        }
    };

    struct BIODeleter {
        void operator()(BIO *bio) const noexcept {
            BIO_free_all(bio);
        }
    };

    using SslCtxPtr = std::unique_ptr<SSL_CTX, SSLContextDeleter>;
    using BioPtr = std::unique_ptr<BIO, BIODeleter>;

    /// Wait for a BIO's underlying socket to become ready.
    /// Returns false on timeout.  Throws DnsLookupException on cancellation.
    [[nodiscard]] bool poll_bio(BIO *bio, PollEvent event, int timeout_ms, int cancel_fd) {
        const int fd = static_cast<int>(BIO_get_fd(bio, nullptr));
        if (fd < 0)
            return false;

        pollfd fds[2] = {};
        fds[0].fd = fd;
        fds[0].events = static_cast<int16_t>(event == PollEvent::READ ? POLLIN : POLLOUT);

        int nfds = 1;
        if (cancel_fd >= 0) {
            fds[1].fd = cancel_fd;
            fds[1].events = POLLIN;
            nfds = 2;
        }

        const int ret = ::poll(fds, static_cast<nfds_t>(nfds), timeout_ms);
        if (ret <= 0)
            return false; // timeout or error

        if (nfds == 2 && (fds[1].revents & POLLIN)) {
            throw DnsLookupException("DoT query cancelled", DnsError::CANCELLED);
        }

        return (fds[0].revents & fds[0].events) != 0;
    }

    // Read exactly n bytes from a BIO using poll() for timeout.
    [[nodiscard]] bool bio_read_exact(BIO *bio, std::span<std::uint8_t> buf, int cancel_fd) {
        while (!buf.empty()) {
            if (!poll_bio(bio, PollEvent::READ, IO_TIMEOUT_MS, cancel_fd)) {
                return false;
            }
            BIO_clear_retry_flags(bio);
            const int rc = BIO_read(bio, buf.data(), static_cast<int>(buf.size()));
            if (rc > 0) {
                buf = buf.subspan(static_cast<size_t>(rc));
            } else if (!BIO_should_retry(bio)) {
                return false;
            }
        }
        return true;
    }

    // Send all bytes over BIO using poll() for timeout.
    [[nodiscard]] bool bio_send_all(BIO *bio, std::span<const std::uint8_t> data, int cancel_fd) {
        while (!data.empty()) {
            if (!poll_bio(bio, PollEvent::WRITE, IO_TIMEOUT_MS, cancel_fd)) {
                return false;
            }
            BIO_clear_retry_flags(bio);
            const int rc = BIO_write(bio, data.data(), static_cast<int>(data.size()));
            if (rc > 0) {
                data = data.subspan(static_cast<size_t>(rc));
            } else if (!BIO_should_retry(bio)) {
                return false;
            }
        }
        return true;
    }

    // ── OpenSSL error handling ──

    [[noreturn]]
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

        throw DnsLookupException(msg, DnsError::CONNECTION);
    }
} // anonymous namespace

// ===========================================================================
//  DotResolver::Impl  —  private implementation
// ===========================================================================

struct DotResolver::Impl {
    // ── Constants ──
    static constexpr auto IDLE_TIMEOUT = 30s;
    static constexpr auto CONNECT_TIMEOUT = 1s;
    static constexpr unsigned char ALPN_DOT[] = {3, 'd', 'o', 't'};

    // ── Static functions ──
    static SslCtxPtr create_ssl_ctx();

    static BioPtr connect(SSL_CTX *ctx, const std::string &server, std::uint16_t port);

    // ── Constructor ──
    explicit Impl(std::string server, std::uint16_t port, std::uint64_t id);

    // ── Member functions ──
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsLookupException> query(
        const std::string &host, RecordKind type, int cancel_fd = -1) const;

    BIO *ensure_connection() const;

    // ── Data members ──
    const std::uint64_t id_;
    const std::string server_;
    const std::uint16_t port_;
    mutable std::mutex mutex_;
    mutable BioPtr persistent_bio_;
    mutable std::chrono::steady_clock::time_point last_use_;
};

DotResolver::Impl::Impl(std::string server, std::uint16_t port, std::uint64_t id)
    : id_(id), server_(std::move(server)), port_(port), last_use_(std::chrono::steady_clock::now()) {
}

std::expected<std::vector<std::uint8_t>, DnsLookupException> DotResolver::Impl::query(
    const std::string &host, RecordKind type, int cancel_fd) const {
    try {
        const auto record_type = DNS::Util::type_to_record_type(type);

        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host,
                     static_cast<std::uint16_t>(record_type));

        // ---- 1. Build the raw DNS query packet ----
        const auto query_bytes = DNS::mkquery(host, record_type);

        // ---- 2. Build the wire format (2-byte length prefix + DNS message) ----
        std::vector<std::uint8_t> wire;
        wire.reserve(2 + query_bytes.size());
        wire.push_back(static_cast<std::uint8_t>(query_bytes.size() >> 8));
        wire.push_back(static_cast<std::uint8_t>(query_bytes.size()));
        wire.insert(wire.end(), query_bytes.begin(), query_bytes.end());

        // ---- 3. Send & receive with connection reuse + auto-reconnect ----
        // The entire I/O is under the mutex so that only one thread touches
        // the shared BIO at a time.  Contention is negligible compared to network round-trip time.
        std::lock_guard lock(mutex_);

        const auto target = fmt::format("{}:{}", server_, port_);

        // Send with one reconnect on failure.
        auto *bio = ensure_connection();
        if (!bio_send_all(bio, std::span{wire}, cancel_fd)) {
            SPDLOG_DEBUG(R"(DoT connection to "{}" lost while sending, reconnecting)", target);
            persistent_bio_.reset();
            bio = ensure_connection();
            if (!bio_send_all(bio, std::span{wire}, cancel_fd)) {
                persistent_bio_.reset();
                throw DnsLookupException(
                    fmt::format(R"(DoT failed to send query to "{}" after reconnect)", target),
                    DnsError::CONNECTION
                );
            }
        }

        SPDLOG_TRACE(R"(DoT sent {} bytes to "{}")", wire.size(), target);

        // ---- 4. Read response ----
        std::uint8_t resp_len_buf[2]{};
        if (!bio_read_exact(bio, resp_len_buf, cancel_fd)) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT failed to read response length from "{}")", target),
                DnsError::CONNECTION
            );
        }

        const std::uint16_t resp_len = Utils::Bytes::read_u16_be(resp_len_buf);
        if (resp_len == 0) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT server "{}" returned zero-length response)", target),
                DnsError::PARSE
            );
        }

        std::vector<std::uint8_t> response(resp_len);
        if (!bio_read_exact(bio, std::span{response}, cancel_fd)) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT failed to read response body from "{}")", target),
                DnsError::CONNECTION
            );
        }

        // Validate DNS response header (RFC 1035 §4.1.1).
        DNS::Validator::validate_response(query_bytes, response);

        last_use_ = std::chrono::steady_clock::now();
        SPDLOG_DEBUG(R"(Resolver #{} DoT query to "{}" succeeded ({} bytes) for "{}")", id_, target, response.size(),
                     host);

        return response;
    } catch (const DnsLookupException &e) {
        return std::unexpected(e);
    }
}

auto DotResolver::Impl::create_ssl_ctx() -> SslCtxPtr {
    SslCtxPtr ctx(SSL_CTX_new(TLS_client_method()));
    if (!ctx)
        throw_ssl_error("SSL_CTX_new");

    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) != 1)
        throw_ssl_error("SSL_CTX_set_min_proto_version");
    if (SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) != 1)
        throw_ssl_error("SSL_CTX_set_max_proto_version");

    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

    // Try OpenSSL's built-in default CA paths first.
    if (SSL_CTX_set_default_verify_paths(ctx.get()) != 1) {
        SPDLOG_DEBUG("SSL_CTX_set_default_verify_paths failed, falling back to cert_util");
        // Fall back to our own CA bundle discovery.
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

    SSL_CTX_set_mode(ctx.get(), SSL_MODE_AUTO_RETRY);

    return ctx;
}

auto DotResolver::Impl::connect(SSL_CTX *ctx, const std::string &server, std::uint16_t port) -> BioPtr {
    BioPtr bio(BIO_new_ssl_connect(ctx));
    if (!bio) {
        throw_ssl_error("BIO_new_ssl_connect");
    }

    bool is_ip = InetAddress::parse(server).has_value();
    if (!is_ip && !Utils::is_valid_domain(server)) {
        throw DnsLookupException(
            fmt::format(R"(Invalid DoT server: "{}" (not a valid IP or domain name))", server),
            DnsError::CONFIG
        );
    }

    SSL *ssl = nullptr;
    BIO_get_ssl(bio.get(), &ssl);
    if (ssl) {
        if (!is_ip) {
            SSL_set_tlsext_host_name(ssl, server.c_str());
            SSL_set1_host(ssl, server.c_str());
        }
        // RFC 7858 §3.2: advertise the "dot" ALPN protocol.
        SSL_set_alpn_protos(ssl, ALPN_DOT, sizeof(ALPN_DOT));
    }

    const auto target = fmt::format("{}:{}", server, port);
    if (BIO_set_conn_hostname(bio.get(), target.c_str()) != 1) {
        throw_ssl_error(fmt::format("BIO_set_conn_hostname({})", target));
    }

    // ---- Non-blocking connect with 1-second timeout ----
    BIO_set_nbio(bio.get(), 1);

    const auto deadline = std::chrono::steady_clock::now() + CONNECT_TIMEOUT;

    for (;;) {
        const auto ret = BIO_do_connect(bio.get());
        if (ret == 1)
            break;

        if (!BIO_should_retry(bio.get())) {
            throw_ssl_error(fmt::format(R"(DoT connect/handshake failed for "{}")", target));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw DnsLookupException(
                fmt::format(R"(DoT connection timeout ({}s) for "{}")", CONNECT_TIMEOUT.count(), target),
                DnsError::CONNECTION
            );
        }

        const auto fd = BIO_get_fd(bio.get(), nullptr);
        if (fd == -1) {
            throw DnsLookupException(
                fmt::format(R"(DoT failed to get socket fd for "{}")", target),
                DnsError::CONNECTION
            );
        }

        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

        // Wait for the socket to become readable/writable via poll().
        pollfd pfd{
            .fd = static_cast<int>(fd),
            .events = static_cast<int16_t>(BIO_should_read(bio.get()) ? POLLIN : POLLOUT),
            .revents = 0,
        };

        const int poll_ret = poll(&pfd, 1, static_cast<int>(remaining_ms.count()));
        if (poll_ret <= 0) {
            if (poll_ret == 0) {
                throw DnsLookupException(
                    fmt::format(R"(DoT connection timeout ({}s) for "{}")", CONNECT_TIMEOUT.count(), target),
                    DnsError::CONNECTION
                );
            }
            throw_ssl_error(fmt::format(R"(DoT poll() failed for "{}")", target));
        }
    }

    // Keep non-blocking mode for data I/O (poll()-based send/recv
    // with timeout and optional cancel_fd support).

    SSL *connected_ssl = nullptr;
    BIO_get_ssl(bio.get(), &connected_ssl);
    SPDLOG_TRACE(R"(DoT TLS connection established to "{}" (tls_version: {})))", target,
                 connected_ssl ? SSL_get_version(connected_ssl) : "?");

    return bio;
}

BIO *DotResolver::Impl::ensure_connection() const {
    const auto now = std::chrono::steady_clock::now();

    if (persistent_bio_) {
        const auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_use_);
        if (idle < IDLE_TIMEOUT) [[likely]] {
            // Quick health check: detect server-side close / RST
            const int fd = static_cast<int>(BIO_get_fd(persistent_bio_.get(), nullptr));
            if (fd != -1) {
                pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
                if (poll(&pfd, 1, 0) > 0) {
                    SPDLOG_TRACE(R"(DoT server closed connection to "{}:{}", reconnecting)", server_, port_);
                    persistent_bio_.reset();
                }
            }
            if (persistent_bio_) {
                return persistent_bio_.get();
            }
        }
        if (persistent_bio_) {
            SPDLOG_TRACE(R"(DoT idle timeout ({}s) for "{}:{}", reconnecting)", idle.count(), server_, port_);
            persistent_bio_.reset();
        }
    }

    static const SslCtxPtr ssl_ctx = create_ssl_ctx();
    persistent_bio_ = connect(ssl_ctx.get(), server_, port_);
    last_use_ = now;
    return persistent_bio_.get();
}

// ===========================================================================
//  DotResolver  —  public API
// ===========================================================================

DotResolver::DotResolver(std::string server, std::uint16_t port)
    : impl_(std::make_unique<Impl>(std::move(server), port, get_id())) {
}

DotResolver::~DotResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsLookupException> DotResolver::query(
    const std::string &host, RecordKind type, int cancel_fd) const noexcept {
    return impl_->query(host, type, cancel_fd);
}

// ===========================================================================
//  Self-registration
// ===========================================================================

namespace {
    [[maybe_unused]] DnsResolverRegistry::Registrar _dot(
        "tls",
        [](const Config::DnsServer &server) -> std::shared_ptr<ResolverBase> {
            auto uri = Uri::parse(server.address);
            return std::make_shared<DotResolver>(std::string(uri.get_host()), uri.get_port());
        });
} // namespace
