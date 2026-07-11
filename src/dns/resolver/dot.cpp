//
// Created by Kotarou on 2026/6/29.
//

#include "dot.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include "util/random.hpp"
#include <span>
#include <string>
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include "exception/dns_lookup.h"
#include "exception/dns_packet.h"
#include "exception/tls.h"
#include "dns/dns_error_info.h"
#include "dns/resolver_registry.h"
#include "dns/util.hpp"
#include "dns/validator.h"
#include "dns/wire/builder.h"
#include "network/tls_connection.h"
#include "util/bytes.hpp"

#include "dns_error.h"
#include "uri.h"

#include "fmt.hpp"
#include <spdlog/spdlog.h>

namespace {
    using namespace std::chrono_literals;
} // anonymous namespace

// ===========================================================================
//  DotResolver::Impl  —  private implementation
// ===========================================================================

struct DotResolver::Impl {
    // ── Constants ──
    static constexpr auto IDLE_TIMEOUT = 30s;
    static constexpr auto CONNECT_TIMEOUT = 1s;
    static constexpr unsigned char ALPN_DOT[] = {3, 'd', 'o', 't'};

    // ── Constructor ──
    explicit Impl(std::string server, std::uint16_t port, std::uint64_t id, std::string label);

    // ── Public member functions ──
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query(
        const std::string &host, RecordKind type, int cancel_fd = -1) const;

    // ── Private helpers ──
    /// Ensure a persistent TLS connection exists (create or reuse).
    /// @return  std::expected<void, DnsErrorInfo> — empty on success, error on failure.
    [[nodiscard]] std::expected<void, DnsErrorInfo> ensure_connection() const;

    /// Build a padded DNS query for DoT (RFC 7858 §3.5 / RFC 7830).
    /// @throws  DnsPacketException on invalid input (programming error).
    [[nodiscard]] static std::vector<std::uint8_t> build_padded_query(const std::string &host, DNS::RecordType type);

    [[nodiscard]] static std::vector<std::uint8_t> build_wire_format(const std::vector<std::uint8_t> &query_bytes);

    /// Send the wire-format query with one automatic reconnect.
    /// @return  std::expected on success or I/O error (timeout, cancellation).
    [[nodiscard]] std::expected<void, DnsErrorInfo> send_query(std::span<const std::uint8_t> wire, int cancel_fd) const;

    /// Read the response (2-byte length prefix + DNS message).
    /// @return  Parsed DNS response on success, or an I/O/parse error.
    ///          Does NOT throw.
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> read_response(int cancel_fd) const;

    // ── Data members ──
    const std::uint64_t id_;
    const std::string server_;
    const std::uint16_t port_;
    const std::string label_;   // display label for log / error messages
    mutable std::mutex mutex_;
    mutable std::unique_ptr<TlsConnection> persistent_conn_;
    mutable std::chrono::steady_clock::time_point last_use_;
    mutable bool alpn_warned_{false};
};

DotResolver::Impl::Impl(std::string server, std::uint16_t port, std::uint64_t id, std::string label)
    : id_(id), server_(std::move(server)), port_(port), label_(std::move(label)), last_use_(std::chrono::steady_clock::now()) {
}

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DotResolver::Impl::query(
    const std::string &host, RecordKind type, int cancel_fd) const {
    try {
        const auto record_type = DNS::Util::type_to_record_type(type);

        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host,
                     static_cast<std::uint16_t>(record_type));

        // ---- 1. Build the padded DNS query packet (RFC 7830) ----
        const auto query_bytes = build_padded_query(host, record_type);

        // ---- 2. Build DoT wire format (2-byte length prefix + DNS message) ----
        const auto wire = build_wire_format(query_bytes);

        // ---- 3. I/O under mutex for shared connection -------
        // Retry once with reconnection on transient I/O failure.
        constexpr int MAX_ATTEMPTS = 2;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            std::lock_guard lock(mutex_);

            if (attempt == 1) {
                SPDLOG_DEBUG(R"(Connection to "{}" failed, reconnecting)", label_);
                // No graceful TLS shutdown needed — we are about to reconnect.
                // The connection may already be in an inconsistent state (e.g.
                // connect timed out mid-handshake, leaving ssl_ dangling).
                // Just close and let ensure_connection() rebuild from scratch.
                persistent_conn_->close();
            }

            auto send_result = send_query(wire, cancel_fd);
            if (!send_result) {
                if (attempt < MAX_ATTEMPTS - 1) continue;
                return std::unexpected(std::move(send_result.error()));
            }

            auto response = read_response(cancel_fd);
            if (!response) {
                // CANCELLED should not be retried — abort immediately.
                if (response.error().code == DnsError::CANCELLED) {
                    return std::unexpected(std::move(response.error()));
                }
                if (attempt < MAX_ATTEMPTS - 1) continue;
                return std::unexpected(std::move(response.error()));
            }

            // ---- 4. Validate DNS response header (RFC 1035 §4.1.1) ----
            auto valid = DNS::Validator::validate_response(query_bytes, *response);
            if (!valid) {
                return std::unexpected(std::move(valid.error()));
            }

            last_use_ = std::chrono::steady_clock::now();
            SPDLOG_DEBUG(R"(Resolver #{} query succeeded ({} bytes) for "{}")", id_, response->size(), host);

            return std::move(*response);
        }

        // Not reached.
        std::unreachable();
    } catch (const DnsPacketException &e) {
        return std::unexpected(DnsErrorInfo{
            DnsError::PARSE,
            fmt::format(R"(Packet construction for "{}" failed: {})", host, e.what())
        });
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
//  build_padded_query  —  build DNS query with EDNS(0) padding (RFC 7830)
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> DotResolver::Impl::build_padded_query(
    const std::string &host, DNS::RecordType type) {
    // RFC 7830 / RFC 7858 §3.5: pad DoT queries to a block boundary to
    // obscure query length and reduce traffic-analysis risk.  A 128-octet
    // block size is a reasonable trade-off between overhead and protection.
    constexpr size_t PAD_BLOCK = 128;

    // EDNS0 OPT pseudo-record + padding-option overhead (excluding the
    // padding bytes themselves):
    //   NAME     1  (root label)
    //   TYPE     2  (OPT = 41)
    //   CLASS    2  (UDP payload size)
    //   TTL      4
    //   RDLENGTH 2
    //   + padding-option code (2) + length (2)  =  4
    //   ─────────────────────────────────────────
    //   Total   15 bytes overhead
    constexpr size_t EDNS_PAD_OVERHEAD = 15;

    // Step 1: build the base query without EDNS0 to know its wire size.
    const auto base = DNS::QueryBuilder{}.add_question(host, type).build();

    // Step 2: calculate padding length needed to reach the next block boundary.
    const size_t raw_size = base.size() + EDNS_PAD_OVERHEAD;
    const size_t pad_len = (raw_size % PAD_BLOCK == 0) ? PAD_BLOCK : PAD_BLOCK - raw_size % PAD_BLOCK;
    // pad_len is guaranteed to be >= 1 because (PAD_BLOCK - raw_size % PAD_BLOCK)
    // is in [1, PAD_BLOCK] when raw_size % PAD_BLOCK != 0, and we substitute
    // PAD_BLOCK (>= 1) when the remainder is zero.

    // Step 3: rebuild with EDNS0 padding option (code 12, RFC 7830).
    // Padding bytes SHOULD be unpredictable (RFC 7830 §3).
    std::vector<std::uint8_t> padding_data(pad_len);
    std::ranges::generate(padding_data, [] { return static_cast<std::uint8_t>(Utils::Random::engine()()); });
    const DNS::EdnsOption pad_opt{12, padding_data};

    return DNS::QueryBuilder{}
            .add_question(host, type)
            .add_edns(512, 0, false, std::span(&pad_opt, 1))
            .build();
}

// ---------------------------------------------------------------------------
//  build_wire_format  —  2-byte length prefix + DNS message
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> DotResolver::Impl::build_wire_format(const std::vector<std::uint8_t> &query_bytes) {
    std::vector<std::uint8_t> wire(2 + query_bytes.size());
    Utils::Bytes::write_u16_be(wire, static_cast<std::uint16_t>(query_bytes.size()));
    std::ranges::copy(query_bytes, wire.begin() + 2);
    return wire;
}

// ---------------------------------------------------------------------------
//  send_query  —  sends with one automatic reconnect + TLS close_notify
//
//  Returns std::expected for I/O errors (cancellation, send failure).
// ---------------------------------------------------------------------------

std::expected<void, DnsErrorInfo> DotResolver::Impl::send_query(
    std::span<const std::uint8_t> wire, int cancel_fd) const {
    if (auto res = ensure_connection(); !res) {
        return std::unexpected(std::move(res.error()));
    }
    auto status = persistent_conn_->send_all(wire, cancel_fd);

    if (!status) {
        if (status.error() == TlsConnection::IoStatus::CANCELLED) {
            [[maybe_unused]] const auto _ = persistent_conn_->shutdown();
            persistent_conn_->close();
            return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "Query cancelled"});
        }
        [[maybe_unused]] const auto _ = persistent_conn_->shutdown();
        persistent_conn_->close();
        return std::unexpected(DnsErrorInfo{
            DnsError::CONNECTION,
            fmt::format(R"(Failed to send query to "{}")", label_)
        });
    }

    SPDLOG_TRACE(R"(Sent {} bytes to "{}")", wire.size(), label_);
    return {};
}

// ---------------------------------------------------------------------------
//  read_response  —  read 2-byte length prefix + body
//
//  Returns std::expected for all errors — I/O and parse errors are expected
//  conditions.  Does NOT throw.
//
//  On error the connection is NOT closed here — the caller propagates the
//  error and ensure_connection() will detect the stale state on the
//  next query and reconnect if needed.
// ---------------------------------------------------------------------------

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DotResolver::Impl::read_response(int cancel_fd) const {
    // Read 2-byte response length prefix (big-endian).
    std::array<std::uint8_t, 2> length_buffer{};
    auto status = persistent_conn_->read_exact(length_buffer, cancel_fd);
    if (!status) {
        if (status.error() == TlsConnection::IoStatus::CANCELLED) {
            return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "Query cancelled"});
        }
        return std::unexpected(DnsErrorInfo{
            DnsError::CONNECTION,
            fmt::format(R"(Failed to read response length from "{}")", label_)
        });
    }

    const auto resp_len = Utils::Bytes::read_u16_be(length_buffer);
    if (resp_len == 0) {
        return std::unexpected(DnsErrorInfo{
            DnsError::PARSE,
            fmt::format(R"(Server "{}" returned zero-length response)", label_)
        });
    }
    constexpr size_t MAX_DOT_RESPONSE_SIZE = 65535;
    if (resp_len > MAX_DOT_RESPONSE_SIZE) {
        return std::unexpected(DnsErrorInfo{
            DnsError::PARSE,
            fmt::format(R"(Server "{}" response too large: {} bytes)", label_, resp_len)
        });
    }

    // Read response body.
    std::vector<std::uint8_t> response(resp_len);
    status = persistent_conn_->read_exact(std::span{response}, cancel_fd);
    if (!status) {
        if (status.error() == TlsConnection::IoStatus::CANCELLED) {
            return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "Query cancelled"});
        }
        return std::unexpected(DnsErrorInfo{
            DnsError::CONNECTION,
            fmt::format(R"(Failed to read response body from "{}")", label_)
        });
    }

    return response;
}

// ---------------------------------------------------------------------------
//  ensure_connection  —  manage connection reuse + ALPN verification
//
//  Returns std::expected<void, DnsErrorInfo> — empty on success, error on failure.
// ---------------------------------------------------------------------------

std::expected<void, DnsErrorInfo> DotResolver::Impl::ensure_connection() const {
    const auto now = std::chrono::steady_clock::now();

    if (persistent_conn_ && persistent_conn_->is_connected()) {
        const auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_use_);
        if (idle < IDLE_TIMEOUT) [[likely]] {
            // Quick health check: detect server-side close / RST
            if (persistent_conn_->is_healthy()) [[likely]] {
                return {};
            }
            SPDLOG_TRACE(R"(Server closed connection to "{}", reconnecting)", label_);
            [[maybe_unused]] auto _ = persistent_conn_->shutdown();
            persistent_conn_->close();
        } else {
            SPDLOG_TRACE(R"(Idle timeout ({}s) for "{}", reconnecting)", idle.count(), label_);
            [[maybe_unused]] auto _ = persistent_conn_->shutdown();
            persistent_conn_->close();
        }
    }

    if (!persistent_conn_) {
        persistent_conn_ = std::make_unique<TlsConnection>(server_, port_, CONNECT_TIMEOUT,
                                                           std::nullopt,
                                                           std::span<const unsigned char>(ALPN_DOT));
    }

    auto connect_result = persistent_conn_->connect();
    if (!connect_result) {
        if (connect_result.error() == TlsConnection::IoStatus::TIMEOUT) {
            return std::unexpected(DnsErrorInfo{DnsError::RETRY,
                fmt::format(R"(Connection to "{}" timed out)", label_)});
        }
        return std::unexpected(DnsErrorInfo{DnsError::CONNECTION,
            fmt::format(R"(Connection to "{}" failed)", label_)});
    }

    // Verify ALPN: the server should have negotiated "dot" (3 bytes).
    // Warn only once per server lifetime to avoid log spam on reconnect.
    if (!alpn_warned_) {
        const auto alpn = persistent_conn_->negotiated_alpn();
        if (alpn != "dot") {
            alpn_warned_ = true;
            SPDLOG_WARN(R"(Server "{}" negotiated unexpected ALPN protocol "{}")",
                        label_, alpn.empty() ? "(none)" : alpn);
        }
    }

    last_use_ = now;
    return {};
}

// ===========================================================================
//  DotResolver  —  public API
// ===========================================================================

DotResolver::DotResolver(std::string server, std::uint16_t port, std::string label)
    : impl_(std::make_unique<Impl>(std::move(server), port, get_id(), std::move(label))) {
}

DotResolver::~DotResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DotResolver::query(
    const std::string &host, RecordKind type, int cancel_fd) const {
    return impl_->query(host, type, cancel_fd);
}

// ===========================================================================
//  Self-registration
// ===========================================================================

namespace {
    [[maybe_unused]] DnsResolverRegistry::Registrar _dot(
        "tls",
        [](const Config::DnsServer &server) -> std::unique_ptr<ResolverBase> {
            auto uri = Uri::parse(server.address);
            return std::make_unique<DotResolver>(std::string(uri.get_host()), uri.get_port(), std::string(uri.get_origin()));
        });
} // namespace
