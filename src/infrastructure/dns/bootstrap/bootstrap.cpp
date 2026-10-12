//
// dns — bootstrap name resolution over the coroutine transport.
//

#include "bootstrap.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <utility>
#include <vector>

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include <expected>
#include <spdlog/spdlog.h>

#include "coro/group.hpp"
#include "coro/scope.hpp"
#include "coro/task_group.hpp"
#include "domain/config/dns_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/network/address_family.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/dns_packet_exception.h"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/logging/level_enabled.h"
#include "support/fmt.hpp"
#include "support/redact.hpp"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace dns {

net::HostResolver make_bootstrap_resolver(std::vector<domain::DnsServer> servers, Policy policy) {
    return [servers = std::move(servers), policy](std::string host, std::optional<domain::AddressFamily> family) {
        return bootstrap_resolve(std::move(host), family, servers, policy);
    };
}

namespace {

/// Outcome of resolving one record kind.
struct KindResult {
    domain::RecordKind kind;
    std::vector<domain::InetAddress> addresses;
    /// Diagnosis of a run that ended with no address. It names `host` because
    /// with_host() passes NODATA through unchanged instead of naming it twice.
    domain::DnsErrorInfo error;

    KindResult(const std::string& host, domain::RecordKind record_kind)
        : kind(record_kind),
          error{domain::DnsError::NODATA, fmt::format(R"(DNS lookup for "{}" returned no records)", host)} {}
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
/// RCODE describes this server's view; negative answers still allow failover.
[[nodiscard]] std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo> extract_addresses(
    const std::vector<std::uint8_t>& response, const std::string& host, const domain::RecordKind kind) {
    const auto expected_type = static_cast<std::uint16_t>(dns::Util::type_to_record_type(kind));
    const auto parsed = dns::RecordParser::parse_response(response, host, dns::Util::type_to_record_type(kind));

    if (auto error = Util::rcode_error(parsed.rcode, host)) {
        return std::unexpected(std::move(*error));
    }

    std::vector<domain::InetAddress> addresses;
    for (const auto& record : parsed.answers) {
        if (record.type != expected_type) {
            continue;
        }
        const auto expected_length = kind == domain::RecordKind::A ? 4U : 16U;
        if (record.rdata.size() != expected_length) {
            return std::unexpected(
                domain::DnsErrorInfo{domain::DnsError::PARSE, "DNS address RDATA length does not match its type"});
        }
        if (auto address = domain::InetAddress::from_bytes(record.rdata)) {
            addresses.push_back(*address);
        }
    }
    return addresses;
}

/// One server attempt is the translation boundary; the scheduler handles errors.
struct ServerResult {
    std::size_t index;
    std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo> outcome;
};

[[nodiscard]] coro::Task<ServerResult> query_server(std::string host, domain::RecordKind kind, domain::DnsServer server,
                                                    std::size_t index, Policy policy) {
    const auto address = domain::InetAddress::parse(server.address);
    if (!address || server.port == 0) {
        // A bootstrap server must be an IP literal with a nonzero port — a
        // config defect for this entry; the remaining servers can still answer.
        // Warn at warn level: the race loop below records the failure only at
        // debug level, and a skipped server is worth seeing without it.
        const auto redacted = Utils::redacted_uri_credentials(server.address);
        const std::string_view defect =
            address ? "must use a nonzero port" : "must be an IP literal with a nonzero port";
        SPDLOG_WARN(R"(Skipping bootstrap DNS server "{}": {})", redacted, defect);
        co_return ServerResult{
            index, std::unexpected(domain::DnsErrorInfo{
                       domain::DnsError::CONFIG, fmt::format(R"(Bootstrap DNS server "{}" {})", redacted, defect)})};
    }
    try {
        const auto query = build_query(host, Util::type_to_record_type(kind));
        auto response = co_await detail::query_classic(*address, server.port, query, policy);
        if (!response)
            co_return ServerResult{index, std::unexpected(std::move(response.error()))};
        auto addresses = extract_addresses(*response, host, kind);
        if (addresses && addresses->empty()) {
            addresses = std::unexpected(domain::DnsErrorInfo{
                domain::DnsError::NODATA, fmt::format(R"(DNS lookup for "{}" returned no usable records)", host)});
        }
        co_return ServerResult{index, std::move(addresses)};
    } catch (const DnsPacketException& error) {
        co_return ServerResult{index,
                               std::unexpected(domain::DnsErrorInfo{domain::DnsError::INVALID_QUERY, error.what()})};
    } catch (const DnsLookupException& error) {
        co_return ServerResult{index, std::unexpected(domain::DnsErrorInfo{error.get_error(), error.what()})};
    }
}

/// Race a bounded set of servers, replacing each failed candidate immediately.
/// Unlike Dispatcher, bootstrap returns typed addresses and never retries a
/// server within this lookup. Both paths reuse the classic wire exchange.
[[nodiscard]] coro::Task<KindResult> resolve_kind(std::string host, const domain::RecordKind kind,
                                                  std::vector<domain::DnsServer> servers, Policy policy) {
    KindResult result{host, kind};
    bool has_failure = false;
    co_await coro::with_cancel_scope([&](coro::CancelScope& race) -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            std::size_t pending = 0;
            const auto launch = [&] {
                if (pending < servers.size()) {
                    group.spawn(query_server(host, kind, servers[pending], pending, policy));
                    ++pending;
                }
            };
            for (std::size_t i = 0; i < std::min(policy.max_concurrent, servers.size()); ++i)
                launch();
            while (auto completed = co_await group.next<ServerResult>()) {
                ServerResult attempt{};
                attempt = std::move(completed->value());
                if (attempt.outcome) {
                    result.addresses = std::move(*attempt.outcome);
                    race.cancel();
                    break;
                }
                auto error = std::move(attempt.outcome.error());
                const auto& server = servers[attempt.index];
                if (logging::level_enabled(spdlog::level::debug)) {
                    SPDLOG_DEBUG(R"(DNS lookup for "{}" via {}:{} failed: {})", host,
                                 Utils::redacted_uri_credentials(server.address), server.port, error.message);
                }
                const bool definitive = error.code == domain::DnsError::INVALID_QUERY;
                if (!has_failure || Util::error_priority(error.code) > Util::error_priority(result.error.code)) {
                    result.error = std::move(error);
                    has_failure = true;
                }
                if (definitive) {
                    race.cancel();
                    break;
                }
                launch();
            }
        });
    });
    co_return result;
}

/// Use the same failure precedence as the dispatcher; positives are handled first.
[[nodiscard]] domain::DnsErrorInfo best_error(const domain::DnsErrorInfo& first, const domain::DnsErrorInfo& second) {
    return Util::error_priority(first.code) >= Util::error_priority(second.code) ? first : second;
}

/// Attach the one fact a per-server error cannot carry: the name being
/// resolved. Transport failures (RETRY/CONNECTION) carry per-server detail —
/// server, port, budget — that is logged at the failure point, so here they
/// collapse to the bare summary. Messages that already name the host
/// (NXDOMAIN, NODATA) pass through unchanged instead of naming it twice.
[[nodiscard]] domain::DnsErrorInfo with_host(const std::string& host, domain::DnsErrorInfo error) {
    switch (error.code) {
        case domain::DnsError::RETRY:
        case domain::DnsError::CONNECTION:
            error.message = fmt::format(R"(Cannot resolve "{}")", host);
            break;
        case domain::DnsError::NX_DOMAIN:
        case domain::DnsError::NODATA:
            break;
        default:
            error.message = fmt::format(R"(Cannot resolve "{}": {})", host, error.message);
            break;
    }
    return error;
}

}  // namespace

coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> bootstrap_resolve(
    std::string host, const std::optional<domain::AddressFamily> family, std::vector<domain::DnsServer> servers,
    Policy policy) {
    if (policy.max_concurrent == 0)
        co_return std::unexpected(domain::DnsErrorInfo{domain::DnsError::CONFIG, "DNS concurrency must be positive"});
    if (const auto literal = domain::InetAddress::parse(host)) {
        if (family && literal->get_family() != *family) {
            // The literal contradicts the family the caller asked for: a local
            // parameter mismatch, not a name that exists without the records.
            co_return std::unexpected(domain::DnsErrorInfo{
                domain::DnsError::CONFIG,
                fmt::format(R"(IP literal "{}" does not match the requested address family)", host)});
        }
        co_return std::vector<domain::InetAddress>{*literal};
    }
    if (servers.empty()) {
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::CONFIG,
                                 fmt::format(R"(Cannot resolve "{}": no bootstrap DNS servers are configured)", host)});
    }

    if (host.empty())
        host = ".";
    const auto kinds = kinds_for(family);
    if (kinds.size() == 1) {
        auto result = co_await resolve_kind(host, kinds.front(), std::move(servers), policy);
        if (result.addresses.empty()) {
            co_return std::unexpected(with_host(host, std::move(result.error)));
        }
        co_return std::move(result.addresses);
    }

    // Once one family succeeds, collect the other within the policy window. A silent
    // family must not discard an address we can already connect to. The local
    // scope cancels and joins unfinished children; ancestor cancellation propagates.
    KindResult ipv4{host, domain::RecordKind::A};
    KindResult ipv6{host, domain::RecordKind::AAAA};
    co_await coro::with_cancel_scope([&](coro::CancelScope& collection) -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(resolve_kind(host, domain::RecordKind::A, servers, policy));
            group.spawn(resolve_kind(host, domain::RecordKind::AAAA, servers, policy));
            const auto collect = [&](KindResult result) {
                const bool usable = !result.addresses.empty();
                (result.kind == domain::RecordKind::A ? ipv4 : ipv6) = std::move(result);
                return usable;
            };
            auto first = co_await group.next<KindResult>();
            if (!first)
                co_return;
            const bool usable = collect(std::move(first->value()));
            if (!usable) {
                if (auto second = co_await group.next<KindResult>())
                    collect(std::move(second->value()));
                co_return;
            }
            [[maybe_unused]] auto window =
                co_await coro::with_timeout(policy.family_collection_window, [&]() -> coro::Task<void> {
                    if (auto second = co_await group.next<KindResult>())
                        collect(std::move(second->value()));
                });
            collection.cancel();
        });
    });

    std::vector<domain::InetAddress> addresses;
    addresses.insert(addresses.end(), ipv4.addresses.begin(), ipv4.addresses.end());
    addresses.insert(addresses.end(), ipv6.addresses.begin(), ipv6.addresses.end());
    if (addresses.empty()) {
        co_return std::unexpected(with_host(host, best_error(ipv4.error, ipv6.error)));
    }
    co_return addresses;
}

}  // namespace dns
