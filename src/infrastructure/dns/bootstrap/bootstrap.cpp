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
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/dns/wire/query_util.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"

namespace dns {

net::HostResolver make_bootstrap_resolver(std::vector<domain::DnsServer> servers) {
    return [servers = std::move(servers)](std::string host, std::optional<domain::AddressFamily> family) {
        return bootstrap_resolve(std::move(host), family, servers);
    };
}

namespace {

/// Outcome of resolving one record kind.
struct KindResult {
    std::vector<domain::InetAddress> addresses;
    domain::DnsErrorInfo error{domain::DnsError::NODATA, "no records"};
};

/// The record kinds a family preference expands to (A first).
[[nodiscard]] std::vector<domain::RecordKind> kinds_for(const std::optional<domain::AddressFamily> family) {
    if (family == domain::AddressFamily::IPV4) {
        return {domain::RecordKind::A};
    }
    if (family == domain::AddressFamily::IPV6) {
        return {domain::RecordKind::AAAA};
    }
    return {domain::RecordKind::A, domain::RecordKind::AAAA};
}

/// Pull the addresses of `type` out of a validated response.
///
/// NXDOMAIN is authoritative for the name and reported as an error.
[[nodiscard]] std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo> extract_addresses(
    const std::vector<std::uint8_t>& response, const std::string& host, const domain::RecordKind kind) {
    const auto expected_type = static_cast<std::uint16_t>(dns::Util::type_to_record_type(kind));
    const auto parsed = dns::RecordParser::parse_response(response, host);

    if (parsed.rcode == dns::Rcode::NXDOMAIN) {
        return std::unexpected(domain::DnsErrorInfo{domain::DnsError::NX_DOMAIN,
                                                    fmt::format(R"(Domain "{}" does not exist (NXDOMAIN))", host)});
    }

    std::vector<domain::InetAddress> addresses;
    for (const auto& record : parsed.answers) {
        if (record.type != expected_type) {
            continue;
        }
        if (auto address = domain::InetAddress::from_bytes(record.rdata)) {
            addresses.push_back(*address);
        }
    }
    return addresses;
}

/// Query one record kind against every server in order.
///
/// A server that cannot answer — transport failure, a malformed response, an
/// authoritative NXDOMAIN, or a NODATA answer — never ends the search: the
/// error is recorded and the next server is tried, because a later server can
/// still hold the records (split-horizon DNS, partial views).
[[nodiscard]] coro::Task<KindResult> resolve_kind(std::string host, const domain::RecordKind kind,
                                                  std::vector<domain::DnsServer> servers) {
    KindResult result;
    result.error =
        domain::DnsErrorInfo{domain::DnsError::NODATA, fmt::format(R"(DNS lookup for "{}" returned no records)", host)};

    for (const domain::DnsServer& server : servers) {
        const auto address = domain::InetAddress::parse(server.address);
        if (!address.has_value()) {
            // A bootstrap server must be an IP literal — a config defect for
            // this entry; the remaining servers can still answer.
            result.error =
                domain::DnsErrorInfo{domain::DnsError::CONFIG,
                                     fmt::format(R"(Bootstrap DNS server "{}" is not an IP literal)", server.address)};
            continue;
        }

        try {
            const auto query = dns::build_query(host, dns::Util::type_to_record_type(kind));
            auto response = co_await detail::query_udp(*address, server.port, query);
            if (!response) {
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
                // NXDOMAIN is authoritative for the name on this server only.
                result.error = std::move(found.error());
                continue;
            }
            if (!found->empty()) {
                result.addresses = std::move(*found);
                co_return result;
            }
            // NODATA: a valid answer with no records of this kind.
        } catch (const std::bad_alloc&) {
            throw;  // an allocation failure is never downgraded to a retryable error
        } catch (const DnsLookupException& error) {
            result.error = domain::DnsErrorInfo{error.get_error(), error.what()};
        } catch (const std::exception& error) {
            result.error = domain::DnsErrorInfo{
                domain::DnsError::PARSE,
                fmt::format(R"(Failed to build or parse a query for "{}": {})", host, error.what())};
        }
    }

    co_return result;
}

/// Adapter so a kind can be resolved by a spawned group child.
[[nodiscard]] coro::Task<void> resolve_into(std::string host, const domain::RecordKind kind,
                                            std::vector<domain::DnsServer> servers, KindResult* out) {
    *out = co_await resolve_kind(std::move(host), kind, std::move(servers));
    co_return;
}

/// Prefer a definitive error, then NXDOMAIN, over a transient one.
[[nodiscard]] domain::DnsErrorInfo best_error(const domain::DnsErrorInfo& first, const domain::DnsErrorInfo& second) {
    if (detail::is_definitive(first.code)) {
        return first;
    }
    if (detail::is_definitive(second.code)) {
        return second;
    }
    if (first.code == domain::DnsError::NX_DOMAIN) {
        return first;
    }
    if (second.code == domain::DnsError::NX_DOMAIN) {
        return second;
    }
    return second;
}

}  // namespace

coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> bootstrap_resolve(
    std::string host, const std::optional<domain::AddressFamily> family, std::vector<domain::DnsServer> servers) {
    if (const auto literal = domain::InetAddress::parse(host)) {
        co_return std::vector<domain::InetAddress>{*literal};
    }
    if (servers.empty()) {
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::CONFIG,
                                 fmt::format(R"(Cannot resolve "{}": no bootstrap DNS servers are configured)", host)});
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
        group.spawn(resolve_into(host, domain::RecordKind::A, servers, &ipv4));
        group.spawn(resolve_into(host, domain::RecordKind::AAAA, servers, &ipv6));
        co_return;
    });

    std::vector<domain::InetAddress> addresses;
    addresses.insert(addresses.end(), ipv4.addresses.begin(), ipv4.addresses.end());
    addresses.insert(addresses.end(), ipv6.addresses.begin(), ipv6.addresses.end());
    if (addresses.empty()) {
        co_return std::unexpected(best_error(ipv4.error, ipv6.error));
    }
    co_return addresses;
}

}  // namespace dns
