//
// Created by Kotarou on 2026/6/29.
//

#include "dot_resolver.h"

#include <atomic>
#include <chrono>
#include <mutex>

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <sys/select.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <spdlog/spdlog.h>

#include "dns_mkquery.h"
#include "exception/dns_lookup_exception.h"
#include "cert_util.h"

namespace {
    struct SSLContextDeleter {
        void operator()(SSL_CTX *ctx) const { SSL_CTX_free(ctx); }
    };
    struct BIODeleter {
        void operator()(BIO *bio) const { BIO_free_all(bio); }
    };
    using SslCtxPtr = std::unique_ptr<SSL_CTX, SSLContextDeleter>;
    using BioPtr = std::unique_ptr<BIO, BIODeleter>;

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
        throw DnsLookupException(msg, dns_lookup_error_type::CONNECTION);
    }

    using Deadline = std::chrono::steady_clock::time_point;

    constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(10);
    constexpr auto IO_TIMEOUT = std::chrono::seconds(10);

    // Wait until the socket is ready for the requested direction(s) or the
    // deadline expires. Returns false when the deadline has passed.
    bool bio_wait(BIO *bio, bool wait_read, bool wait_write, const Deadline &deadline) {
        const int fd = BIO_get_fd(bio, nullptr);
        if (fd < 0) {
            return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
        fd_set read_fds;
        fd_set write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        if (wait_read) FD_SET(fd, &read_fds);
        if (wait_write) FD_SET(fd, &write_fds);
        struct timeval tv{
            static_cast<time_t>(remaining.count() / 1000000),
            static_cast<suseconds_t>(remaining.count() % 1000000)
        };
        return select(fd + 1, wait_read ? &read_fds : nullptr, wait_write ? &write_fds : nullptr, nullptr, &tv) > 0;
    }

    bool bio_read_exact(BIO *bio, uint8_t *buf, size_t n, const Deadline &deadline) {
        while (n > 0) {
            const int rc = BIO_read(bio, buf, static_cast<int>(n));
            if (rc <= 0) {
                if (BIO_should_retry(bio)) {
                    // If OpenSSL already has decrypted data buffered, skip the
                    // poll and retry the read immediately.
                    SSL *ssl = nullptr;
                    BIO_get_ssl(bio, &ssl);
                    if (ssl && SSL_pending(ssl) > 0) {
                        continue;
                    }
                    if (bio_wait(bio, true, false, deadline)) {
                        continue;
                    }
                }
                return false;
            }
            buf += rc;
            n -= static_cast<size_t>(rc);
        }
        return true;
    }

    bool bio_send_all(BIO *bio, const uint8_t *data, size_t n, const Deadline &deadline) {
        while (n > 0) {
            const int rc = BIO_write(bio, data, static_cast<int>(n));
            if (rc <= 0) {
                if (BIO_should_retry(bio) && bio_wait(bio, false, true, deadline)) continue;
                return false;
            }
            data += rc;
            n -= static_cast<size_t>(rc);
        }
        return true;
    }

    SslCtxPtr create_ssl_ctx() {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) throw_ssl_error("SSL_CTX_new");
        SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_verify_depth(ctx, 4);
        auto ca_path = get_system_ca_path();
        if (!ca_path.empty()) {
            SSL_CTX_load_verify_locations(ctx, ca_path.data(), nullptr);
            SPDLOG_DEBUG("Loaded CA bundle from {}", ca_path);
        } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
            SPDLOG_DEBUG("SSL_CTX_set_default_verify_paths failed");
        }
        SSL_CTX_set_cipher_list(ctx, "HIGH:!aNULL:!eNULL:!MD5");
        return SslCtxPtr(ctx);
    }

    SslCtxPtr &global_ssl_ctx() {
        static SslCtxPtr ctx = create_ssl_ctx();
        return ctx;
    }

    uint64_t next_id() {
        static std::atomic<uint64_t> id{0};
        return id.fetch_add(1, std::memory_order_relaxed);
    }
} // anonymous namespace

class DotResolver::Impl {
public:
    explicit Impl(std::string server, uint16_t port)
        : id_(next_id()), server_(std::move(server)), port_(port) {
    }

    std::vector<uint8_t> query(const std::string &host, int ns_type) const {
        if (ns_type == ns_t_invalid) {
            throw DnsLookupException("Unsupported dns_type for DoT query", dns_lookup_error_type::UNKNOWN);
        }

        SPDLOG_DEBUG(R"(DoT #{} lookup for domain "{}" (type {}))", id_, host, ns_type);

        const auto query_bytes = dns_mkquery(host, ns_type);

        // 2-byte big-endian length prefix + DNS message (RFC 7858 §3.3)
        uint8_t len_buf[2];
        len_buf[0] = static_cast<uint8_t>(query_bytes.size() >> 8 & 0xFF);
        len_buf[1] = static_cast<uint8_t>(query_bytes.size() & 0xFF);

        std::vector<uint8_t> wire;
        wire.reserve(2 + query_bytes.size());
        wire.insert(wire.end(), len_buf, len_buf + 2);
        wire.insert(wire.end(), query_bytes.begin(), query_bytes.end());

        std::lock_guard lock(mutex_);
        const auto target = fmt::format("{}:{}", server_, port_);

        auto *bio = ensure_connection();
        if (!bio_send_all(bio, wire.data(), wire.size(), std::chrono::steady_clock::now() + IO_TIMEOUT)) {
            SPDLOG_DEBUG(R"(DoT: connection to "{}" lost, reconnecting)", target);
            persistent_bio_.reset();
            bio = ensure_connection();
            if (!bio_send_all(bio, wire.data(), wire.size(), std::chrono::steady_clock::now() + IO_TIMEOUT)) {
                persistent_bio_.reset();
                throw DnsLookupException(
                    fmt::format(R"(DoT: failed to send query to "{}" after reconnect)", target),
                    dns_lookup_error_type::CONNECTION);
            }
        }

        SPDLOG_TRACE(R"(DoT: sent {} bytes to "{}")", wire.size(), target);

        uint8_t resp_len_buf[2];
        if (!bio_read_exact(bio, resp_len_buf, 2, std::chrono::steady_clock::now() + IO_TIMEOUT)) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT: failed to read response length from "{}")", target),
                dns_lookup_error_type::CONNECTION);
        }

        const uint16_t resp_len = read_uint16_be(resp_len_buf);
        if (resp_len == 0) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT: server "{}" returned empty response)", target),
                dns_lookup_error_type::NODATA);
        }

        std::vector<uint8_t> response(resp_len);
        if (!bio_read_exact(bio, response.data(), resp_len, std::chrono::steady_clock::now() + IO_TIMEOUT)) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT: failed to read response body from "{}")", target),
                dns_lookup_error_type::CONNECTION);
        }

        last_use_ = std::chrono::steady_clock::now();
        SPDLOG_DEBUG(R"(DoT #{}: query to "{}" succeeded ({} bytes) for "{}")", id_, target, response.size(), host);
        return response;
    }

private:
    static constexpr auto IDLE_TIMEOUT = std::chrono::seconds(30);

    BIO *ensure_connection() const {
        auto &ctx = global_ssl_ctx();
        if (!ctx) {
            throw DnsLookupException("DoT: SSL context not initialized", dns_lookup_error_type::CONNECTION);
        }

        if (persistent_bio_) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_use_ > IDLE_TIMEOUT) {
                SPDLOG_TRACE(R"(DoT: connection to "{}:{}" idle timeout)", server_, port_);
                persistent_bio_.reset();
            }
        }

        if (!persistent_bio_) {
            const auto target = fmt::format("{}:{}", server_, port_);
            SPDLOG_TRACE(R"(DoT: connecting to "{}")", target);

            BIO *bio = BIO_new_ssl_connect(ctx.get());
            if (!bio) {
                ERR_clear_error();
                throw DnsLookupException(
                    fmt::format(R"(DoT: failed to create SSL connection to "{}")", target),
                    dns_lookup_error_type::CONNECTION);
            }

            BioPtr bio_ptr(bio);
            SSL *ssl = nullptr;
            BIO_get_ssl(bio, &ssl);
            if (!ssl) throw_ssl_error("BIO_get_ssl");

            SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

            // Verify the peer identity against the configured server: hostname
            // verification for names, IP address matching for literals
            // (SNI is not sent for IP literals, RFC 6066 §3).
            in_addr addr4{};
            in6_addr addr6{};
            auto *verify_param = SSL_get0_param(ssl);
            if (inet_pton(AF_INET, server_.c_str(), &addr4) == 1 ||
                inet_pton(AF_INET6, server_.c_str(), &addr6) == 1) {
                if (verify_param == nullptr ||
                    X509_VERIFY_PARAM_set1_ip_asc(verify_param, server_.c_str()) != 1) {
                    throw_ssl_error("X509_VERIFY_PARAM_set1_ip_asc");
                }
            } else {
                SSL_set_tlsext_host_name(ssl, server_.c_str());
                if (verify_param == nullptr || SSL_set1_host(ssl, server_.c_str()) != 1) {
                    throw_ssl_error("SSL_set1_host");
                }
            }

            BIO_set_conn_hostname(bio, target.c_str());
            BIO_set_conn_port(bio, std::to_string(port_).c_str());
            BIO_set_nbio(bio, 1);

            const auto connect_deadline = std::chrono::steady_clock::now() + CONNECT_TIMEOUT;

            int rc;
            while ((rc = BIO_do_connect(bio)) <= 0) {
                if (!BIO_should_retry(bio) || !bio_wait(bio, false, true, connect_deadline)) {
                    ERR_clear_error();
                    throw DnsLookupException(
                        fmt::format(R"(DoT: failed to connect to "{}")", target),
                        dns_lookup_error_type::CONNECTION);
                }
            }

            while ((rc = BIO_do_handshake(bio)) <= 0) {
                if (!BIO_should_retry(bio) || !bio_wait(bio, true, true, connect_deadline)) {
                    ERR_clear_error();
                    throw DnsLookupException(
                        fmt::format(R"(DoT: SSL handshake failed for "{}")", target),
                        dns_lookup_error_type::CONNECTION);
                }
            }

            SPDLOG_DEBUG(R"(DoT: connected to "{}" (#{}))", target, id_);
            persistent_bio_ = std::move(bio_ptr);
            last_use_ = std::chrono::steady_clock::now();
        }

        return persistent_bio_.get();
    }

    const uint64_t id_;
    const std::string server_;
    const uint16_t port_;
    mutable std::mutex mutex_;
    mutable BioPtr persistent_bio_;
    mutable std::chrono::steady_clock::time_point last_use_;
};

DotResolver::DotResolver(std::string server, uint16_t port)
    : impl_(std::make_unique<Impl>(std::move(server), port)) {
}

DotResolver::~DotResolver() = default;

std::vector<uint8_t> DotResolver::query(const std::string &host, int ns_type) const {
    return impl_->query(host, ns_type);
}
