//
// dns — classic resolver.
//

#include "classic.h"

#include <exception>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/query_util.h"
#include "support/fmt.hpp"

namespace dns {

coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> ClassicResolver::query(
    std::string host, const domain::RecordKind kind) {
    try {
        SPDLOG_TRACE(R"(Resolver #{} DNS lookup for "{}")", id(), host);

        const auto record_type = dns::Util::type_to_record_type(kind);
        SPDLOG_DEBUG(R"(Resolver #{} Resolving "{}" (type {}) via {}:{})", id(), host,
                     static_cast<std::uint16_t>(record_type), server_.to_string(), port_);

        const auto query_bytes = dns::build_query(host, record_type);

        auto response = co_await detail::query_udp(server_, port_, query_bytes);
        if (!response) {
            co_return std::unexpected(std::move(response.error()));
        }
        if (auto valid = dns::Validator::validate_response(query_bytes, *response); !valid) {
            co_return std::unexpected(std::move(valid.error()));
        }
        if (!detail::is_truncated(*response)) {
            co_return std::move(*response);
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
        co_return std::move(*over_tcp);
    } catch (const std::bad_alloc&) {
        throw;  // allocation failures are never downgraded to a retryable error
    } catch (const DnsLookupException& error) {
        co_return std::unexpected(domain::DnsErrorInfo{error.get_error(), error.what()});
    } catch (const std::exception& error) {
        co_return std::unexpected(domain::DnsErrorInfo{
            domain::DnsError::PARSE, fmt::format(R"(Classic DNS query for "{}" failed: {})", host, error.what())});
    }
}

}  // namespace dns
