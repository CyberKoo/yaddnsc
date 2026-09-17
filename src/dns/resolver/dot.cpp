//
// Created by Kotarou on 2026/6/29.
//
// DNS-over-TLS resolver (RFC 7858) on Transport.
//

#include "dot.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/random.hpp"

#include "dns/dns_error_info.h"
#include "exception/dns_lookup.h"
#include "exception/dns_packet.h"
#include "dns/util.hpp"
#include "dns/validator.h"
#include "dns/wire/builder.h"
#include "network/transport/stream.h"
#include "network/transport/tls_stream.h"
#include "util/bytes.hpp"
#include "util/cancellation_token.hpp"

#include "dns_error.h"
#include "uri.h"

#include "fmt.hpp"
#include <spdlog/spdlog.h>

namespace {
    using namespace std::chrono_literals;

    /// Map a transport I/O error to DnsErrorInfo (post-connect I/O stage).
    [[nodiscard]] DnsErrorInfo map_io_error(const Transport::IoError err, const std::string_view label,
                                            const std::string_view stage) {
        using enum Transport::IoError;
        switch (err) {
            case CANCELLED:
                return {DnsError::CANCELLED, "Query cancelled"};
            case TIMEOUT:
            case CONNECTION_FAILED:
                return {DnsError::CONNECTION, fmt::format(R"(Failed to {} from "{}")", stage, label)};
        }
        return {DnsError::CONNECTION, fmt::format(R"(Failed to {} from "{}")", stage, label)};
    }

    /// Map a connect-stage error.
    [[nodiscard]] DnsErrorInfo map_connect_error(const Transport::IoError err, const std::string_view label) {
        using enum Transport::IoError;
        switch (err) {
            case CANCELLED:
                return {DnsError::CANCELLED, "Query cancelled"};
            case TIMEOUT:
                return {DnsError::RETRY, fmt::format(R"(Connection to "{}" timed out)", label)};
            case CONNECTION_FAILED:
                return {DnsError::CONNECTION, fmt::format(R"(Connection to "{}" failed)", label)};
        }
        return {DnsError::CONNECTION, fmt::format(R"(Connection to "{}" failed)", label)};
    }
} // namespace

// ===========================================================================
//  DotResolver::Impl  —  private implementation
// ===========================================================================

struct DotResolver::Impl {
    // ── Constants ──
    static constexpr auto CONNECT_TIMEOUT = 1s;
    static constexpr unsigned char ALPN_DOT[] = {3, 'd', 'o', 't'};

    /// Connection + TLS options for the DoT connection.
    [[nodiscard]] static std::pair<Transport::Options, Transport::TlsOptions> make_tls_options() {
        Transport::Options conn;
        conn.connect_timeout = CONNECT_TIMEOUT;
        Transport::TlsOptions tls;
        tls.alpn_proto = ALPN_DOT;
        return {conn, tls};
    }

    /// Production ctor: creates the TLS stream with the token bound.
    Impl(std::string server, std::uint16_t port, std::uint64_t id, std::string label,
         Utils::CancellationToken token);

    /// Testing ctor: stream injected.
    Impl(std::string server, std::uint16_t port, std::uint64_t id, std::string label,
         std::unique_ptr<Transport::Stream> stream);

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type) const;

    /// Build a padded DNS query for DoT (RFC 7858 §3.5 / RFC 7830).
    /// @throws  DnsPacketException on invalid input (programming error).
    [[nodiscard]] static std::vector<std::uint8_t> build_padded_query(const std::string &host, DNS::RecordType type);

    [[nodiscard]] static std::vector<std::uint8_t> build_wire_format(const std::vector<std::uint8_t> &query_bytes);

    /// Read the response (2-byte length prefix + DNS message).
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> read_response() const;

    // ── Data members ──
    const std::uint64_t id_;
    const std::string server_;
    const std::uint16_t port_;
    const std::string label_;   // display label for log / error messages
    mutable std::mutex mutex_;
    mutable std::unique_ptr<Transport::Stream> stream_;
};

DotResolver::Impl::Impl(std::string server, const std::uint16_t port, const std::uint64_t id, std::string label,
                        Utils::CancellationToken token)
    : id_(id), server_(std::move(server)), port_(port), label_(std::move(label)),
      stream_(std::make_unique<Transport::TlsStream>(server_, port_, make_tls_options().first,
                                                     make_tls_options().second, std::move(token))) {
}

DotResolver::Impl::Impl(std::string server, const std::uint16_t port, const std::uint64_t id, std::string label,
                        std::unique_ptr<Transport::Stream> stream)
    : id_(id), server_(std::move(server)), port_(port), label_(std::move(label)), stream_(std::move(stream)) {
}

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DotResolver::Impl::query(
    const std::string &host, RecordKind type) const {
    try {
        const auto record_type = DNS::Util::type_to_record_type(type);

        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host,
                     static_cast<std::uint16_t>(record_type));

        // ---- 1. Build the padded DNS query packet (RFC 7830) ----
        const auto query_bytes = build_padded_query(host, record_type);

        // ---- 2. Build DoT wire format (2-byte length prefix + DNS message) ----
        const auto wire = build_wire_format(query_bytes);

        // ---- 3. I/O under mutex for the shared stream -------
        // Retry once with reconnection on transient I/O failure.
        constexpr int MAX_ATTEMPTS = 2;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            std::lock_guard lock(mutex_);

            if (attempt == 1) {
                SPDLOG_DEBUG(R"(Connection to "{}" failed, reconnecting)", label_);
                stream_->close();
            }

            // ensure_connected() is idempotent: healthy → no-op, stale → rebuild.
            if (auto connected = stream_->ensure_connected(); !connected) {
                stream_->close();
                if (connected.error() == Transport::IoError::CANCELLED) {
                    return std::unexpected(map_connect_error(connected.error(), label_));
                }
                if (attempt < MAX_ATTEMPTS - 1) {
                    continue;
                }
                return std::unexpected(map_connect_error(connected.error(), label_));
            }

            if (auto sent = stream_->send_all(wire); !sent) {
                stream_->close();
                if (sent.error() == Transport::IoError::CANCELLED) {
                    return std::unexpected(map_io_error(sent.error(), label_, "send query"));
                }
                if (attempt < MAX_ATTEMPTS - 1) {
                    continue;
                }
                return std::unexpected(map_io_error(sent.error(), label_, "send query"));
            }

            SPDLOG_TRACE(R"(Sent {} bytes to "{}")", wire.size(), label_);

            auto response = read_response();
            if (!response) {
                stream_->close();
                if (response.error().code == DnsError::CANCELLED) {
                    return std::unexpected(std::move(response.error()));
                }
                if (attempt < MAX_ATTEMPTS - 1) {
                    continue;
                }
                return std::unexpected(std::move(response.error()));
            }

            // ---- 4. Validate DNS response header (RFC 1035 §4.1.1) ----
            auto valid = DNS::Validator::validate_response(query_bytes, *response);
            if (!valid) {
                return std::unexpected(std::move(valid.error()));
            }

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
//  read_response  —  read 2-byte length prefix + body
// ---------------------------------------------------------------------------

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DotResolver::Impl::read_response() const {
    // Read 2-byte response length prefix (big-endian).
    std::array<std::uint8_t, 2> length_buffer{};
    if (auto status = stream_->read_exact(length_buffer); !status) {
        return std::unexpected(map_io_error(status.error(), label_, "read response length"));
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
    std::vector<std::uint8_t> response(resp_len, 0);
    if (auto status = stream_->read_exact(std::span{response}); !status) {
        return std::unexpected(map_io_error(status.error(), label_, "read response body"));
    }

    return response;
}

// ===========================================================================
//  DotResolver  —  public API
// ===========================================================================

DotResolver::DotResolver(std::string server, const std::uint16_t port, std::string label,
                         Utils::CancellationToken token)
    : impl_(std::make_unique<Impl>(std::move(server), port, get_id(), std::move(label), std::move(token))) {
}

DotResolver::DotResolver(std::string server, const std::uint16_t port, std::string label,
                         std::unique_ptr<Transport::Stream> stream)
    : impl_(std::make_unique<Impl>(std::move(server), port, get_id(), std::move(label), std::move(stream))) {
}

DotResolver::~DotResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
DotResolver::query(const std::string &host, RecordKind type) const {
    return impl_->query(host, type);
}
