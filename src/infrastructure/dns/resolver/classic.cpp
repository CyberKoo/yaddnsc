//
// dns — classic resolver.
//

#include "classic.h"

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <spdlog/spdlog.h>
#include <expected>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/logging/level_enabled.h"
#include "support/fmt.hpp"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "coro/task.hpp"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace dns {

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> ClassicResolver::query(
    std::string host, const domain::RecordKind kind) {
    try {
        SPDLOG_TRACE(R"(Resolver #{} DNS lookup for "{}")", id(), host);

        const auto record_type = dns::Util::type_to_record_type(kind);
        // to_string() allocates — build the endpoint only for a written record.
        if (logging::level_enabled(spdlog::level::trace)) {
            SPDLOG_TRACE(R"(Resolver #{} Resolving "{}" (type {}) via {}:{})", id(), host,
                         static_cast<std::uint16_t>(record_type), server_.to_string(), port_);
        }

        const auto query_bytes = dns::build_query(host, record_type);

        auto udp = co_await detail::query_udp(server_, port_, query_bytes);
        if (!udp) {
            co_return std::unexpected(std::move(udp.error()));
        }

        // Kernel-level truncation (the datagram exceeded the receive buffer)
        // needs the same TCP retry as a TC-bit answer, but the partial bytes
        // must never be validated or parsed.
        bool truncated = udp->truncated;
        if (!truncated) {
            if (auto valid = dns::Validator::validate_response(query_bytes, udp->bytes); !valid) {
                co_return std::unexpected(std::move(valid.error()));
            }
            truncated = detail::is_truncated(udp->bytes);
        }
        if (!truncated) {
            co_return std::move(udp->bytes);
        }

        // A truncated UDP answer is retried over TCP against the same server.
        SPDLOG_TRACE(R"(Resolver #{} UDP response truncated for "{}", falling back to TCP)", id(), host);
        auto over_tcp = co_await detail::query_tcp(server_, port_, query_bytes);
        if (!over_tcp) {
            co_return std::unexpected(std::move(over_tcp.error()));
        }
        if (auto valid = dns::Validator::validate_response(query_bytes, *over_tcp); !valid) {
            co_return std::unexpected(std::move(valid.error()));
        }
        if (detail::is_truncated(*over_tcp)) {
            // TC on a TCP answer is a broken server, not a transport condition:
            // the retry already ran, so the answer cannot be completed. RFC 7766
            // §8 only governs the TCP length prefix, so treating this as RETRY is
            // this implementation's policy — the dispatcher moves to the next
            // backend instead of parsing an incomplete message.
            SPDLOG_WARN(R"(Resolver #{} TCP response for "{}" still has TC set; treating it as a broken server)", id(),
                        host);
            co_return std::unexpected(
                domain::DnsErrorInfo{domain::DnsError::RETRY,
                                     fmt::format(R"(DNS server {}:{} returned a truncated TCP response for "{}")",
                                                 server_.to_string(), port_, host)});
        }
        co_return std::move(*over_tcp);
    } catch (const std::bad_alloc&) {
        throw;  // allocation failures are never downgraded to a retryable error
    } catch (const DnsLookupException& error) {
        co_return std::unexpected(domain::DnsErrorInfo{error.get_error(), error.what()});
    } catch (const DnsPacketException& error) {
        // Packet construction failure is a definitive input defect (e.g. an
        // over-long name), never a transient transport error — the dispatcher
        // must not burn a retry on it (same classification as bootstrap).
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::PARSE, fmt::format(R"(Failed to build a DNS query for "{}": {})", host, error.what())});
    } catch (const std::exception& error) {
        // An unexpected exception is a transient defect of this attempt, not a
        // deterministic parse failure: UNKNOWN keeps the dispatcher falling
        // over to the next backend instead of abandoning the search. Warn so
        // the defect is visible — nothing below this catch reports it.
        SPDLOG_WARN(R"(Resolver #{} unexpected exception while querying "{}": {})", id(), host, error.what());
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::UNKNOWN, fmt::format(R"(Classic DNS query for "{}" failed: {})", host, error.what())});
    }
}

}  // namespace dns
