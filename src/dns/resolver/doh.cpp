//
// Created by Kotarou on 2026/6/28.
//
#include "doh.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dns/util.hpp"
#include "dns/validator.h"
#include "dns/wire/query_util.h"
#include "dns/dns_error_info.h"
#include "exception/dns_lookup.h"
#include "network/tls_connection.h"
#include "dns/resolver_registry.h"
#include "http/http.h"
#include "network/transport/tls_stream.h"

#include "dns_error.h"
#include "uri.h"
#include "version.h"

#include "fmt.hpp"
#include <spdlog/spdlog.h>

namespace {
/// Map Http::Error to DnsErrorInfo for DoH.
[[nodiscard]] DnsErrorInfo to_dns_error(Http::Error err, std::string_view label) {
    switch (err) {
    case Http::Error::CANCELLED:
        return {DnsError::CANCELLED, "Query cancelled"};
    case Http::Error::TIMEOUT:
    case Http::Error::CONNECTION_FAILED:
        return {DnsError::CONNECTION,
                fmt::format(R"(Failed to read response from "{}")", label)};
    default:
        return {DnsError::PARSE,
                fmt::format(R"(Server "{}" returned malformed HTTP response)", label)};
    }
}
} // anonymous namespace

namespace {
    using namespace std::chrono_literals;

    /// Build a proper HTTP Host header value per RFC 7230 §5.4.
    /// Omits the port when it is the HTTPS default (443) and brackets IPv6 addresses.
    [[nodiscard]] std::string build_host_header(std::string_view host, std::uint16_t port) {
        const bool is_ipv6 = host.find(':') != std::string_view::npos;
        if (port == 443) {
            return is_ipv6 ? fmt::format("[{}]", host) : std::string(host);
        }
        return is_ipv6 ? fmt::format("[{}]:{}", host, port) : fmt::format("{}:{}", host, port);
    }
} // anonymous namespace

// ===========================================================================
//  DohResolver::Impl  —  private implementation
// ===========================================================================

struct DohResolver::Impl {
    // ── Constants ──
    static constexpr auto IDLE_TIMEOUT = 30s;
    static constexpr auto CONNECT_TIMEOUT = 1s;
    static constexpr unsigned char ALPN_HTTP[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

    // ── Constructor ──
    explicit Impl(std::string server, std::uint16_t port, std::string path, std::uint64_t id, std::string label);

    // ── Public member functions ──
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type,
          const Utils::CancellationToken &cancel_token) const;

    // ── Private helpers ──
    /// Ensure a persistent TLS connection exists (create or reuse).
    /// @return  std::expected<void, DnsErrorInfo> — empty on success, error on failure.
    [[nodiscard]] std::expected<void, DnsErrorInfo> ensure_connection() const;

    // ── Data members ──
    const std::uint64_t id_;
    const std::string server_;
    const std::uint16_t port_;
    const std::string path_;
    const std::string host_header_;
    const std::string label_;   // display label for log / error messages
    mutable std::mutex mutex_;
    mutable std::unique_ptr<TlsConnection> persistent_conn_;
    mutable std::chrono::steady_clock::time_point last_use_;
};

DohResolver::Impl::Impl(std::string server, std::uint16_t port, std::string path, std::uint64_t id, std::string label)
    : id_(id), server_(std::move(server)), port_(port), path_(std::move(path)),
      host_header_(build_host_header(server_, port_)), label_(std::move(label)), last_use_(std::chrono::steady_clock::now()) {
}

// ===========================================================================
//  Impl::query  —  orchestrator
// ===========================================================================

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DohResolver::Impl::query(
    const std::string &host, RecordKind type,
    const Utils::CancellationToken &cancel_token) const {
    try {
        const auto record_type = DNS::Util::type_to_record_type(type);

        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host,
                     static_cast<std::uint16_t>(record_type));

        // ---- 1. Build the raw DNS query packet ----
        const auto query_bytes = DNS::build_query(host, record_type);

        // ---- 2. Build HTTP POST request (RFC 8484) ----
        HttpRequest req;
        req.method = HttpMethod::POST;
        req.content_type = "application/dns-message";
        req.headers.emplace("Accept", "application/dns-message");
        req.headers.emplace("Connection", "keep-alive");
        req.body = std::string(reinterpret_cast<const char *>(query_bytes.data()), query_bytes.size());

        // ---- 3. I/O under mutex for shared connection -------
        // Retry once with reconnection on transient I/O failure.
        constexpr int MAX_ATTEMPTS = 2;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            std::lock_guard lock(mutex_);

            if (attempt == 1) {
                SPDLOG_DEBUG(R"(Connection to "{}" failed, reconnecting)", label_);
                persistent_conn_->close();
            }

            // Ensure connection before exchange.
            if (auto res = ensure_connection(); !res) {
                if (attempt < MAX_ATTEMPTS - 1) continue;
                return std::unexpected(std::move(res.error()));
            }

            // Execute full HTTP exchange: build → send → read.
            Transport::TlsStream stream(*persistent_conn_);
            auto response = Http::exchange(stream, path_, req, host_header_, YADDNSC::get_full_version(), cancel_token);
            if (!response) {
                persistent_conn_->close();
                if (response.error() == Http::Error::CANCELLED) {
                    return std::unexpected(to_dns_error(response.error(), label_));
                }
                if (attempt < MAX_ATTEMPTS - 1) continue;
                return std::unexpected(to_dns_error(response.error(), label_));
            }

            // Check HTTP status code (RFC 8484 §4.2.1 — only 200 is valid).
            if (response->status_code != 200) {
                persistent_conn_->close();
                return std::unexpected(DnsErrorInfo{
                    response->status_code >= 500 ? DnsError::RETRY : DnsError::SERVER_REFUSED,
                    fmt::format(R"(Server "{}" returned HTTP status {})", label_, response->status_code)
                });
            }

            // ---- 4. Validate DNS response header (RFC 8484 §5.1 / RFC 1035 §4.1.1) ----
            auto valid = DNS::Validator::validate_response(query_bytes, response->body);
            if (!valid) {
                return std::unexpected(std::move(valid.error()));
            }

            last_use_ = std::chrono::steady_clock::now();
            SPDLOG_DEBUG(R"(Resolver #{} query succeeded ({} bytes) for "{}"))", id_, response->body.size(), host);

            return std::move(response->body);
        }

        // Not reached.
        std::unreachable();
    } catch (const DnsLookupException &e) {
        return std::unexpected(DnsErrorInfo{e.get_error(), e.what()});
    } catch (const std::exception &e) {
        return std::unexpected(DnsErrorInfo{
            DnsError::UNKNOWN,
            fmt::format(R"(Query for "{}" failed: {})", host, e.what())
        });
    }
}

// ===========================================================================
//  Helper implementations
// ===========================================================================

// ---------------------------------------------------------------------------
//  ensure_connection  —  manage connection reuse with idle timeout
//
//  Returns std::expected<void, DnsErrorInfo> — empty on success, error on failure.
// ---------------------------------------------------------------------------

std::expected<void, DnsErrorInfo> DohResolver::Impl::ensure_connection() const {
    const auto now = std::chrono::steady_clock::now();

    if (persistent_conn_ && persistent_conn_->is_connected()) {
        const auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_use_);
        if (idle < IDLE_TIMEOUT) [[likely]] {
            if (persistent_conn_->is_healthy()) [[likely]] {
                return {};
            }
            SPDLOG_TRACE(R"(Server closed connection to "{}", reconnecting)", label_);
            persistent_conn_->close();
        } else {
            SPDLOG_TRACE(R"(Idle timeout ({}s) for "{}", reconnecting)", idle.count(), label_);
            persistent_conn_->close();
        }
    }

    if (!persistent_conn_) {
        persistent_conn_ = std::make_unique<TlsConnection>(
            server_, port_, TlsOptions{.alpn_proto = ALPN_HTTP, .connect_timeout = CONNECT_TIMEOUT}
        );
    }

    auto result = persistent_conn_->connect();
    if (!result) {
        if (result.error() == TlsConnection::IoStatus::TIMEOUT) {
            return std::unexpected(DnsErrorInfo{DnsError::RETRY,
                fmt::format(R"(Connection to "{}" timed out)", label_)});
        }
        return std::unexpected(DnsErrorInfo{DnsError::CONNECTION,
            fmt::format(R"(Connection to "{}" failed)", label_)});
    }

    last_use_ = now;
    return {};
}

// ===========================================================================
//  DohResolver  —  public API
// ===========================================================================

DohResolver::DohResolver(std::string host, std::uint16_t port, std::string path, std::string label)
    : impl_(std::make_unique<Impl>(std::move(host), port, std::move(path), get_id(), std::move(label))) {
}

DohResolver::~DohResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
DohResolver::query(const std::string &host, RecordKind type,
                   const Utils::CancellationToken &cancel_token) const {
    return impl_->query(host, type, cancel_token);
}

// ===========================================================================
//  Self-registration
// ===========================================================================

namespace {
    // DoH resolver: port is read from the URI only; server.port is intentionally
    // ignored because the URI already specifies the port (e.g. https://1.1.1.1:1443/dns-query).
    // If no port is present in the URI, the default is 443.
    [[maybe_unused]] DnsResolverRegistry::Registrar _doh(
        "https",
        [](const Config::DnsServer &server) -> std::unique_ptr<ResolverBase> {
            auto uri = Uri::parse(server.address);
            auto host = std::string(uri.get_host());
            auto port = static_cast<std::uint16_t>(uri.get_port() != 0 ? uri.get_port() : 443);
            auto path = std::string(uri.get_path());
            if (path.empty()) {
                path = "/";
            }
            return std::make_unique<DohResolver>(std::move(host), port, std::move(path), std::string(uri.get_origin()));
        });
} // namespace
