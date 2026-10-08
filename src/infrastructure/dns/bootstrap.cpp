//
// dns — bootstrap name resolution over the coroutine transport.
//

#include "bootstrap.h"

#include <cstdint>
#include <exception>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "infrastructure/coro/group.hpp"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/resolver.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/dns/wire/query_util.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"

namespace dns {
namespace {

/// Outcome of resolving one record kind.
struct KindResult {
    std::vector<InetAddress> addresses;
    DnsErrorInfo error{DnsError::NODATA, "no records"};
};

/// The record kinds a family preference expands to (A first).
[[nodiscard]] std::vector<RecordKind> kinds_for(const std::optional<AddressFamily> family) {
    if (family == AddressFamily::IPV4) {
        return {RecordKind::A};
    }
    if (family == AddressFamily::IPV6) {
        return {RecordKind::AAAA};
    }
    return {RecordKind::A, RecordKind::AAAA};
}

/// Pull the addresses of `type` out of a validated response.
///
/// NXDOMAIN is authoritative for the name and reported as an error.
[[nodiscard]] std::expected<std::vector<InetAddress>, DnsErrorInfo> extract_addresses(
    const std::vector<std::uint8_t>& response, const std::string& host, const RecordKind kind) {
    const auto expected_type = static_cast<std::uint16_t>(dns::Util::type_to_record_type(kind));
    const auto parsed = dns::RecordParser::parse_response(response, host);

    if (parsed.rcode == dns::Rcode::NXDOMAIN) {
        return std::unexpected(
            DnsErrorInfo{DnsError::NX_DOMAIN, fmt::format(R"(Domain "{}" does not exist (NXDOMAIN))", host)});
    }

    std::vector<InetAddress> addresses;
    for (const auto& record : parsed.answers) {
        if (record.type != expected_type) {
            continue;
        }
        if (auto address = InetAddress::from_bytes(record.rdata)) {
            addresses.push_back(*address);
        }
    }
    return addresses;
}

/// Query one record kind against every server in order.
[[nodiscard]] coro::Task<KindResult> resolve_kind(std::string host, const RecordKind kind,
                                                  std::vector<Config::DnsServer> servers) {
    KindResult result;
    result.error = DnsErrorInfo{DnsError::NODATA, fmt::format(R"(DNS lookup for "{}" returned no records)", host)};

    try {
        for (const Config::DnsServer& server : servers) {
            const auto address = InetAddress::parse(server.address);
            if (!address.has_value()) {
                // A bootstrap server must be an IP literal — a config defect, not
                // a transient failure, so it stops the search.
                result.error = DnsErrorInfo{
                    DnsError::CONFIG, fmt::format(R"(Bootstrap DNS server "{}" is not an IP literal)", server.address)};
                co_return result;
            }

            const auto query = dns::build_query(host, dns::Util::type_to_record_type(kind));
            auto response = co_await detail::query_udp(*address, server.port, query);
            if (!response) {
                if (response.error().code == DnsError::CANCELLED) {
                    result.error = std::move(response.error());
                    co_return result;
                }
                result.error = std::move(response.error());
                continue;
            }

            if (auto valid = dns::Validator::validate_response(query, *response); !valid) {
                result.error = std::move(valid.error());
                continue;
            }

            if (detail::is_truncated(*response)) {
                auto over_tcp = co_await detail::query_tcp(*address, server.port, query);
                if (!over_tcp) {
                    if (over_tcp.error().code == DnsError::CANCELLED) {
                        result.error = std::move(over_tcp.error());
                        co_return result;
                    }
                    result.error = std::move(over_tcp.error());
                    continue;
                }
                if (auto valid = dns::Validator::validate_response(query, *over_tcp); !valid) {
                    result.error = std::move(valid.error());
                    continue;
                }
                response = std::move(over_tcp);
            }

            auto found = extract_addresses(*response, host, kind);
            if (!found) {
                // NXDOMAIN is authoritative for the name: no other server can
                // answer this kind either.
                result.error = std::move(found.error());
                co_return result;
            }
            result.addresses = std::move(*found);
            co_return result;
        }
    } catch (const std::bad_alloc&) {
        throw;  // an allocation failure is never downgraded to a retryable error
    } catch (const DnsLookupException& error) {
        result.error = DnsErrorInfo{error.get_error(), error.what()};
        co_return result;
    } catch (const std::exception& error) {
        result.error = DnsErrorInfo{
            DnsError::PARSE, fmt::format(R"(Failed to build or parse a query for "{}": {})", host, error.what())};
        co_return result;
    }

    co_return result;
}

/// Adapter so a kind can be resolved by a spawned group child.
[[nodiscard]] coro::Task<void> resolve_into(std::string host, const RecordKind kind,
                                            std::vector<Config::DnsServer> servers, KindResult* out) {
    *out = co_await resolve_kind(std::move(host), kind, std::move(servers));
    co_return;
}

/// Prefer a definitive error, then NXDOMAIN, over a transient one.
[[nodiscard]] DnsErrorInfo best_error(const DnsErrorInfo& first, const DnsErrorInfo& second) {
    if (detail::is_definitive(first.code)) {
        return first;
    }
    if (detail::is_definitive(second.code)) {
        return second;
    }
    if (first.code == DnsError::NX_DOMAIN) {
        return first;
    }
    if (second.code == DnsError::NX_DOMAIN) {
        return second;
    }
    return second;
}

}  // namespace

coro::Task<std::expected<std::vector<InetAddress>, DnsErrorInfo>> bootstrap_resolve(
    std::string host, const std::optional<AddressFamily> family, std::vector<Config::DnsServer> servers) {
    if (const auto literal = InetAddress::parse(host)) {
        co_return std::vector<InetAddress>{*literal};
    }
    if (servers.empty()) {
        co_return std::unexpected(DnsErrorInfo{
            DnsError::CONFIG, fmt::format(R"(Cannot resolve "{}": no bootstrap DNS servers are configured)", host)});
    }

    const auto kinds = kinds_for(family);
    if (kinds.size() == 1) {
        auto result = co_await resolve_kind(host, kinds.front(), std::move(servers));
        if (result.addresses.empty()) {
            co_return std::unexpected(std::move(result.error));
        }
        co_return std::move(result.addresses);
    }

    // No family preference: A and AAAA are queried concurrently and each keeps
    // what it finds, so a server that answers only one family still contributes.
    KindResult ipv4;
    KindResult ipv6;
    co_await coro::task_group([&host, &servers, &ipv4, &ipv6](coro::TaskGroup& group) -> coro::Task<void> {
        group.spawn(resolve_into(host, RecordKind::A, servers, &ipv4));
        group.spawn(resolve_into(host, RecordKind::AAAA, servers, &ipv6));
        co_return;
    });

    std::vector<InetAddress> addresses;
    addresses.insert(addresses.end(), ipv4.addresses.begin(), ipv4.addresses.end());
    addresses.insert(addresses.end(), ipv6.addresses.begin(), ipv6.addresses.end());
    if (addresses.empty()) {
        co_return std::unexpected(best_error(ipv4.error, ipv6.error));
    }
    co_return addresses;
}

}  // namespace dns
