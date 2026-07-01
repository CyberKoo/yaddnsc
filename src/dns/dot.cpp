//
// Created by Kotarou on 2026/6/29.
//

#include "dot.h"

#include <arpa/nameser.h>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <sys/select.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

#include "types.h"
#include "fmt.hpp"
#include "util/cert_util.h"
#include "dns_mkquery.h"
#include "util/validation.h"
#include "network/inet_address.h"
#include "exception/dns_lookup_exception.h"

namespace {
    // ── RAII wrappers for OpenSSL resources ──

    struct SSLContextDeleter {
        void operator()(SSL_CTX *ctx) const { SSL_CTX_free(ctx); }
    };

    struct BIODeleter {
        void operator()(BIO *bio) const { BIO_free_all(bio); }
    };

    using SslCtxPtr = std::unique_ptr<SSL_CTX, SSLContextDeleter>;
    using BioPtr = std::unique_ptr<BIO, BIODeleter>;

    // ── helpers ──

    uint16_t read_uint16_be(const uint8_t *buf) {
        return (static_cast<uint16_t>(buf[0]) << 8) | static_cast<uint16_t>(buf[1]);
    }

    [[noreturn]] void throw_ssl_error(const std::string &context) {
        std::string msg = context + " [";

        unsigned long err;
        int first = 1;
        while ((err = ERR_get_error()) != 0) {
            if (!first) msg += "; ";
            first = 0;

            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            msg += buf;
        }
        msg += "]";

        throw DnsLookupException(msg, dns_error_type::CONNECTION);
    }

    // Read exactly n bytes from a BIO.  Returns false on EOF / error.
    bool bio_read_exact(BIO *bio, uint8_t *buf, size_t n) {
        while (n > 0) {
            const int rc = BIO_read(bio, buf, static_cast<int>(n));
            if (rc <= 0) {
                if (BIO_should_retry(bio)) {
                    continue;
                }
                return false;
            }
            buf += rc;
            n -= static_cast<size_t>(rc);
        }
        return true;
    }

    // Send all bytes over BIO.
    bool bio_send_all(BIO *bio, const uint8_t *data, size_t n) {
        while (n > 0) {
            const int rc = BIO_write(bio, data, static_cast<int>(n));
            if (rc <= 0) {
                if (BIO_should_retry(bio)) {
                    continue;
                }
                return false;
            }
            data += rc;
            n -= static_cast<size_t>(rc);
        }
        return true;
    }
} // anonymous namespace

// ===========================================================================
//  DotResolver::Impl  —  private implementation
// ===========================================================================

class DotResolver::Impl {
public:
    explicit Impl(std::string server, uint16_t port, uint64_t id) : id_(id), server_(std::move(server)), port_(port) {
    }

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host, dns_type type) const {
        const auto ns_type = DNS::to_ns_type(type);
        if (ns_type == ns_t_invalid) {
            throw DnsLookupException(
                fmt::format(R"(Unsupported dns_type for DoT query: "{}")", host),
                dns_error_type::UNKNOWN
            );
        }

        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host, ns_type);

        // ---- 1. Build the raw DNS query packet ----
        const auto query_bytes = dns_mkquery(host, ns_type);

        // ---- 2. Build the wire format (2-byte length prefix + DNS message) ----
        uint8_t len_buf[2];
        len_buf[0] = static_cast<uint8_t>(query_bytes.size() >> 8 & 0xFF);
        len_buf[1] = static_cast<uint8_t>(query_bytes.size() & 0xFF);

        std::vector<uint8_t> wire;
        wire.reserve(2 + query_bytes.size());
        wire.insert(wire.end(), len_buf, len_buf + 2);
        wire.insert(wire.end(), query_bytes.begin(), query_bytes.end());

        // ---- 3. Send & receive with connection reuse + auto-reconnect ----
        // The entire I/O is under the mutex so that only one thread touches
        // the shared BIO at a time.  Contention is negligible compared to network round-trip time.
        std::lock_guard lock(mutex_);

        const auto target = fmt::format("{}:{}", server_, port_);

        // Send with one reconnect on failure.
        auto *bio = ensure_connection();
        if (!bio_send_all(bio, wire.data(), wire.size())) {
            SPDLOG_DEBUG(R"(DoT: connection to "{}" lost while sending, reconnecting)", target);
            persistent_bio_.reset();
            bio = ensure_connection();
            if (!bio_send_all(bio, wire.data(), wire.size())) {
                persistent_bio_.reset();
                throw DnsLookupException(
                    fmt::format(R"(DoT: failed to send query to "{}" after reconnect)", target),
                    dns_error_type::CONNECTION
                );
            }
        }

        SPDLOG_TRACE(R"(DoT: sent {} bytes to "{}")", wire.size(), target);

        // ---- 4. Read response ----
        uint8_t resp_len_buf[2];
        if (!bio_read_exact(bio, resp_len_buf, 2)) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT: failed to read response length from "{}")", target),
                dns_error_type::CONNECTION
            );
        }

        const uint16_t resp_len = read_uint16_be(resp_len_buf);
        if (resp_len == 0) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT: server "{}" returned empty response)", target),
                dns_error_type::NODATA
            );
        }

        std::vector<uint8_t> response(resp_len);
        if (!bio_read_exact(bio, response.data(), resp_len)) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT: failed to read response body from "{}")", target),
                dns_error_type::CONNECTION
            );
        }

        last_use_ = std::chrono::steady_clock::now();
        SPDLOG_DEBUG(R"(Resolver #{} DoT: query to "{}" succeeded ({} bytes) for "{}")", id_, target, response.size(),
                     host);

        return response;
    }

private:
    // Idle timeout — if a connection has been unused for longer than this,
    // it will be closed and re-established on the next query.
    static constexpr auto IDLE_TIMEOUT = std::chrono::seconds(30);

    // ── SSL_CTX shared across all DotResolver instances ──
    static SslCtxPtr create_ssl_ctx() {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) throw_ssl_error("SSL_CTX_new");

        if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
            SSL_CTX_free(ctx);
            throw_ssl_error("SSL_CTX_set_min_proto_version");
        }
        if (SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1) {
            SSL_CTX_free(ctx);
            throw_ssl_error("SSL_CTX_set_max_proto_version");
        }

        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

        // Try OpenSSL's built-in default CA paths first.
        if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
            SPDLOG_DEBUG("SSL_CTX_set_default_verify_paths failed, falling back to cert_util");
            // Fall back to our own CA bundle discovery.
            const auto ca_path = CertUtil::get_system_ca_path();
            if (ca_path.has_value()) {
                if (SSL_CTX_load_verify_locations(ctx, ca_path->c_str(), nullptr) != 1) {
                    SSL_CTX_free(ctx);
                    throw_ssl_error(fmt::format("SSL_CTX_load_verify_locations({})", *ca_path));
                }
                SPDLOG_DEBUG("Loaded CA bundle from {}", *ca_path);
            } else {
                SSL_CTX_free(ctx);
                throw_ssl_error("SSL_CTX_set_default_verify_paths and cert_util both failed");
            }
        }

        SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);

        return SslCtxPtr(ctx);
    }

    // Open and return a new TLS connection to the DoT server.
    // Ownership stays in the returned BioPtr; the caller must not free it.
    static BioPtr connect(SSL_CTX *ctx, const std::string &server, uint16_t port) {
        BioPtr bio(BIO_new_ssl_connect(ctx));
        if (!bio) {
            throw_ssl_error("BIO_new_ssl_connect");
        }

        bool is_ip = InetAddress::parse(server).has_value();
        if (!is_ip && !Util::is_valid_domain(server)) {
            throw DnsLookupException(
                fmt::format(R"msg(Invalid DoT server: "{}" (not a valid IP or domain name))msg", server),
                dns_error_type::CONNECTION);
        }

        if (!is_ip) {
            SSL *ssl = nullptr;
            BIO_get_ssl(bio.get(), &ssl);
            if (ssl) {
                SSL_set_tlsext_host_name(ssl, server.c_str());
                SSL_set1_host(ssl, server.c_str());
            }
        }

        const auto target = fmt::format("{}:{}", server, port);
        if (BIO_set_conn_hostname(bio.get(), target.c_str()) != 1) {
            throw_ssl_error(fmt::format("BIO_set_conn_hostname({})", target));
        }

        // ---- Non-blocking connect with 1-second timeout ----
        BIO_set_nbio(bio.get(), 1);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        for (;;) {
            const int ret = BIO_do_connect(bio.get());
            if (ret == 1) break;

            if (!BIO_should_retry(bio.get())) {
                throw_ssl_error(fmt::format(R"(DoT: connect/handshake failed for "{}")", target));
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw DnsLookupException(
                    fmt::format(R"(DoT: connection timeout (1s) for "{}")", target),
                    dns_error_type::CONNECTION);
            }

            const int fd = BIO_get_fd(bio.get(), nullptr);
            if (fd == -1) {
                throw DnsLookupException(
                    fmt::format(R"(DoT: failed to get socket fd for "{}")", target),
                    dns_error_type::CONNECTION);
            }

            struct timeval tv;
            const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            tv.tv_sec = static_cast<time_t>(remaining_ms.count() / 1000);
            tv.tv_usec = static_cast<suseconds_t>((remaining_ms.count() % 1000) * 1000);

            fd_set read_fds, write_fds;
            FD_ZERO(&read_fds);
            FD_ZERO(&write_fds);

            if (BIO_should_read(bio.get()))
                FD_SET(fd, &read_fds);
            else
                FD_SET(fd, &write_fds);

            const int sel_ret = select(fd + 1, &read_fds, &write_fds, nullptr, &tv);
            if (sel_ret <= 0) {
                if (sel_ret == 0) {
                    throw DnsLookupException(
                        fmt::format(R"(DoT: connection timeout (1s) for "{}")", target),
                        dns_error_type::CONNECTION);
                }
                throw_ssl_error(fmt::format(R"(DoT: select() failed for "{}")", target));
            }
        }

        // Restore blocking mode for regular I/O.
        BIO_set_nbio(bio.get(), 0);

        SSL *connected_ssl = nullptr;
        BIO_get_ssl(bio.get(), &connected_ssl);
        SPDLOG_TRACE(R"(DoT TLS connection established to "{}" (tls_version: {})))", target,
                     connected_ssl ? SSL_get_version(connected_ssl) : "?");

        return bio;
    }

    // Ensure a usable connection exists.  Returns the cached BIO if it is
    // still fresh, or establishes a new one if none exists or the idle
    // timeout has expired.
    // Caller must hold mutex_.
    BIO *ensure_connection() const {
        const auto now = std::chrono::steady_clock::now();

        if (persistent_bio_) {
            const auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_use_);
            if (idle < IDLE_TIMEOUT) {
                return persistent_bio_.get();
            }
            SPDLOG_TRACE(R"(DoT: idle timeout ({}s) for "{}":{} , reconnecting)", idle.count(), server_, port_);
            persistent_bio_.reset();
        }

        static const SslCtxPtr ssl_ctx = create_ssl_ctx();
        persistent_bio_ = connect(ssl_ctx.get(), server_, port_);
        last_use_ = now;
        return persistent_bio_.get();
    }

    const uint64_t id_;
    const std::string server_;
    const uint16_t port_;

    // ---- mutable: shared state protected by mutex_ ----
    mutable std::mutex mutex_;
    mutable BioPtr persistent_bio_;
    mutable std::chrono::steady_clock::time_point last_use_;
};

// ===========================================================================
//  DotResolver  —  public API
// ===========================================================================

DotResolver::DotResolver(std::string server, uint16_t port) : impl_(
    std::make_unique<Impl>(std::move(server), port, get_id())) {
}

DotResolver::~DotResolver() = default;

std::vector<uint8_t> DotResolver::query(const std::string &host, dns_type type) const {
    return impl_->query(host, type);
}


