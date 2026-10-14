//
// dns — classic resolver.
//

#include "classic.h"

#include <new>
#include <span>

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include <expected>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/logging/level_enabled.h"
#include "support/fmt.hpp"
#include "support/util/random.hpp"
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

        co_return co_await detail::query_classic(server_, port_, query_bytes, policy_);
    } catch (const std::bad_alloc&) {
        throw;  // allocation failures are never downgraded to a retryable error
    } catch (const Utils::Random::EntropyError& error) {
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONNECTION, error.what()});
    } catch (const DnsPacketException& error) {
        // Packet construction failure is a definitive input defect (e.g. an
        // over-long name), never a transient transport error — the dispatcher
        // must not burn a retry on it (same classification as bootstrap).
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY,
                                 fmt::format(R"(Failed to build a DNS query for "{}": {})", host, error.what())});
    }
}

}  // namespace dns
