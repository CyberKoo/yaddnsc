//
// Created by Kotarou on 2026/6/29.
//

#include "dns/resolver/dot.h"

#include <arpa/nameser.h>

#include <span>
#include <mutex>
#include <chrono>
#include <string>
#include <vector>
#include <string_view>
#include <source_location>

#include <poll.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "dns_error.h"
#include "dns/util.hpp"
#include "util/cert_util.hpp"
#include "util/validation.hpp"
#include "dns/proto/mkquery.h"
#include "network/inet_address.h"
#include "exception/dns_lookup.h"

namespace {
    using namespace std::chrono_literals;

    // ── RAII wrappers for OpenSSL resources ──

    struct SSLContextDeleter {
        void operator()(SSL_CTX *ctx) const noexcept { SSL_CTX_free(ctx); }
    };

    struct BIODeleter {
        void operator()(BIO *bio) const noexcept { BIO_free_all(bio); }
    };

    using SslCtxPtr = std::unique_ptr<SSL_CTX, SSLContextDeleter>;
    using BioPtr = std::unique_ptr<BIO, BIODeleter>;

    // ── Helpers ──

    /// Read a 16-bit big-endian value from a raw buffer.
    /// Correct on any platform (no alignment concerns).
    [[nodiscard]] constexpr uint16_t read_uint16_be(const uint8_t *buf) noexcept {
        return (static_cast<uint16_t>(buf[0]) << 8) | static_cast<uint16_t>(buf[1]);
    }

    // ── Error handling ──

    [[noreturn]]
    void throw_ssl_error(std::string_view context, const std::source_location &loc = std::source_location::current()) {
        std::string msg;
        msg.reserve(256);
        msg += context;
        msg += " [";

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

        msg += fmt::format(" (at {}:{}:{})", loc.file_name(), loc.line(), loc.column());

        throw DnsLookupException(msg, DNS::Error::CONNECTION);
    }

    // Read exactly n bytes from a BIO.  Returns false on EOF / error.
    [[nodiscard]] bool bio_read_exact(BIO *bio, std::span<uint8_t> buf) noexcept {
        while (!buf.empty()) {
            const int rc = BIO_read(bio, buf.data(), static_cast<int>(buf.size()));
            if (rc <= 0) [[unlikely]] {
                if (BIO_should_retry(bio)) [[likely]] {
                    continue;
                }
                return false;
            }
            buf = buf.subspan(rc);
        }
        return true;
    }

    // Send all bytes over BIO.
    [[nodiscard]] bool bio_send_all(BIO *bio, std::span<const uint8_t> data) noexcept {
        while (!data.empty()) {
            const int rc = BIO_write(bio, data.data(), static_cast<int>(data.size()));
            if (rc <= 0) [[unlikely]] {
                if (BIO_should_retry(bio)) [[likely]] {
                    continue;
                }
                return false;
            }
            data = data.subspan(rc);
        }
        return true;
    }
} // anonymous namespace

// ===========================================================================
//  DotResolver::Impl  —  private implementation
// ===========================================================================

struct DotResolver::Impl {
    explicit Impl(std::string server, uint16_t port, uint64_t id);

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host, DNS::Type type) const;

    // Idle timeout — if a connection has been unused for longer than this,
    // it will be closed and re-established on the next query.
    static constexpr auto IDLE_TIMEOUT = 30s;

    // ── SSL_CTX shared across all DotResolver instances ──
    static SslCtxPtr create_ssl_ctx();

    // Open and return a new TLS connection to the DoT server.
    // Ownership stays in the returned BioPtr; the caller must not free it.
    static BioPtr connect(SSL_CTX *ctx, const std::string &server, uint16_t port);

    // Ensure a usable connection exists.  Returns the cached BIO if it is
    // still fresh, or establishes a new one if none exists or the idle
    // timeout has expired.
    // Caller must hold mutex_.
    BIO *ensure_connection() const;

    const uint64_t id_;
    const std::string server_;
    const uint16_t port_;

    // ---- mutable: shared state protected by mutex_ ----
    mutable std::mutex mutex_;
    mutable BioPtr persistent_bio_;
    mutable std::chrono::steady_clock::time_point last_use_;
};

DotResolver::Impl::Impl(std::string server, uint16_t port, uint64_t id) : id_(id), server_(std::move(server)), port_(port) {
}

std::vector<uint8_t> DotResolver::Impl::query(const std::string &host, DNS::Type type) const {
    const auto ns_type = DNS::to_ns_type(type);
    if (ns_type == ns_t_invalid) {
        throw DnsLookupException(
            fmt::format(R"(Unsupported DNS::Type for DoT query: "{}")", host),
            DNS::Error::UNKNOWN
        );
    }

    SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host, ns_type);

    // ---- 1. Build the raw DNS query packet ----
    const auto query_bytes = DNS::mkquery(host, ns_type);

    // ---- 2. Build the wire format (2-byte length prefix + DNS message) ----
    std::vector<uint8_t> wire;
    wire.reserve(2 + query_bytes.size());
    wire.push_back(static_cast<uint8_t>(query_bytes.size() >> 8));
    wire.push_back(static_cast<uint8_t>(query_bytes.size()));
    wire.insert(wire.end(), query_bytes.begin(), query_bytes.end());

    // ---- 3. Send & receive with connection reuse + auto-reconnect ----
    // The entire I/O is under the mutex so that only one thread touches
    // the shared BIO at a time.  Contention is negligible compared to network round-trip time.
    std::lock_guard lock(mutex_);

    const auto target = fmt::format("{}:{}", server_, port_);

    // Send with one reconnect on failure.
    auto *bio = ensure_connection();
    if (!bio_send_all(bio, std::span{wire})) {
        SPDLOG_DEBUG(R"(DoT: connection to "{}" lost while sending, reconnecting)", target);
        persistent_bio_.reset();
        bio = ensure_connection();
        if (!bio_send_all(bio, std::span{wire})) {
            persistent_bio_.reset();
            throw DnsLookupException(
                fmt::format(R"(DoT: failed to send query to "{}" after reconnect)", target),
                DNS::Error::CONNECTION
            );
        }
    }

    SPDLOG_TRACE(R"(DoT: sent {} bytes to "{}")", wire.size(), target);

    // ---- 4. Read response ----
    uint8_t resp_len_buf[2]{};
    if (!bio_read_exact(bio, resp_len_buf)) {
        persistent_bio_.reset();
        throw DnsLookupException(
            fmt::format(R"(DoT: failed to read response length from "{}")", target),
            DNS::Error::CONNECTION
        );
    }

    const uint16_t resp_len = read_uint16_be(resp_len_buf);
    if (resp_len == 0) {
        persistent_bio_.reset();
        throw DnsLookupException(
            fmt::format(R"(DoT: server "{}" returned empty response)", target),
            DNS::Error::NODATA
        );
    }

    std::vector<uint8_t> response(resp_len);
    if (!bio_read_exact(bio, std::span{response})) {
        persistent_bio_.reset();
        throw DnsLookupException(
            fmt::format(R"(DoT: failed to read response body from "{}")", target),
            DNS::Error::CONNECTION
        );
    }

    last_use_ = std::chrono::steady_clock::now();
    SPDLOG_DEBUG(R"(Resolver #{} DoT: query to "{}" succeeded ({} bytes) for "{}")", id_, target, response.size(),
                 host);

    return response;
}

auto DotResolver::Impl::create_ssl_ctx() -> SslCtxPtr {
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
        if (!Utils::Cert::get_system_ca_path()
            .and_then([ctx](const auto &path) -> std::optional<std::string> {
                if (SSL_CTX_load_verify_locations(ctx, path.c_str(), nullptr) != 1) {
                    return std::nullopt;
                }
                SPDLOG_DEBUG("Loaded CA bundle from {}", path);
                return path;
            })) {
            SSL_CTX_free(ctx);
            throw_ssl_error("SSL_CTX_set_default_verify_paths and cert_util both failed");
        }
    }

    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);

    return SslCtxPtr(ctx);
}

auto DotResolver::Impl::connect(SSL_CTX *ctx, const std::string &server, uint16_t port) -> BioPtr {
    BioPtr bio(BIO_new_ssl_connect(ctx));
    if (!bio) {
        throw_ssl_error("BIO_new_ssl_connect");
    }

    bool is_ip = InetAddress::parse(server).has_value();
    if (!is_ip && !Utils::is_valid_domain(server)) {
        throw DnsLookupException(
            fmt::format(R"(Invalid DoT server: "{}" (not a valid IP or domain name))", server),
            DNS::Error::CONNECTION);
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

    const auto deadline = std::chrono::steady_clock::now() + 1s;

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
                DNS::Error::CONNECTION);
        }

        const int fd = BIO_get_fd(bio.get(), nullptr);
        if (fd == -1) {
            throw DnsLookupException(
                fmt::format(R"(DoT: failed to get socket fd for "{}")", target),
                DNS::Error::CONNECTION
            );
        }

        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

        // Wait for the socket to become readable/writable via poll().
        pollfd pfd{
            .fd = fd,
            .events = static_cast<int16_t>(BIO_should_read(bio.get()) ? POLLIN : POLLOUT),
            .revents = 0,
        };

        const int poll_ret = poll(&pfd, 1, static_cast<int>(remaining_ms.count()));
        if (poll_ret <= 0) {
            if (poll_ret == 0) {
                throw DnsLookupException(
                    fmt::format(R"(DoT: connection timeout (1s) for "{}")", target),
                    DNS::Error::CONNECTION
                );
            }
            throw_ssl_error(fmt::format(R"(DoT: poll() failed for "{}")", target));
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

BIO *DotResolver::Impl::ensure_connection() const {
    const auto now = std::chrono::steady_clock::now();

    if (persistent_bio_) {
        const auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_use_);
        if (idle < IDLE_TIMEOUT) [[likely]] {
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

// ===========================================================================
//  DotResolver  —  public API
// ===========================================================================

DotResolver::DotResolver(std::string server, uint16_t port) : impl_(
    std::make_unique<Impl>(std::move(server), port, get_id())) {
}

DotResolver::~DotResolver() = default;

std::vector<uint8_t> DotResolver::query(const std::string &host, DNS::Type type) const {
    return impl_->query(host, type);
}
