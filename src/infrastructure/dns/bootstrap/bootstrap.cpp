//
// dns — bootstrap name resolution over the coroutine transport.
//

#include "bootstrap.h"

#include <chrono>
#include <cstdint>
#include <new>
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
#include "coro/sleep.hpp"
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

net::HostResolver make_bootstrap_resolver(std::vector<domain::DnsServer> servers) {
    return [servers = std::move(servers)](std::string host, std::optional<domain::AddressFamily> family) {
        return bootstrap_resolve(std::move(host), family, servers);
    };
}

namespace {

/// Outcome of resolving one record kind.
struct KindResult {
    std::vector<domain::InetAddress> addresses;
    /// Diagnosis of a run that ended with no address. It names `host` because
    /// with_host() passes NODATA through unchanged instead of naming it twice.
    domain::DnsErrorInfo error;

    explicit KindResult(const std::string& host)
        : error{domain::DnsError::NODATA, fmt::format(R"(DNS lookup for "{}" returned no records)", host)} {}
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

/// Query one record kind against every server in order.
///
/// A server that cannot answer — transport failure, a malformed response, an
/// authoritative NXDOMAIN, or a NODATA answer — never ends the search: the
/// error is recorded and the next server is tried, because a later server can
/// still hold the records (split-horizon DNS, partial views).
[[nodiscard]] coro::Task<KindResult> resolve_kind(std::string host, const domain::RecordKind kind,
                                                  std::vector<domain::DnsServer> servers) {
    KindResult result{host};

    // A per-server failure keeps its full transport detail at debug level, so
    // the message handed to with_host can stay terse. A later transient
    // failure must not bury an earlier structural/configuration diagnosis.
    bool has_failure = false;
    const auto record_failure = [&result, &host, &has_failure](const domain::DnsServer& server,
                                                               domain::DnsErrorInfo error) {
        if (logging::level_enabled(spdlog::level::debug)) {
            SPDLOG_DEBUG(R"(DNS lookup for "{}" via "{}":{} failed: {})", host,
                         Utils::redacted_uri_credentials(server.address), server.port, error.message);
        }
        if (has_failure && Util::error_priority(result.error.code) >= Util::error_priority(error.code)) {
            return;
        }
        result.error = std::move(error);
        has_failure = true;
    };

    for (const domain::DnsServer& server : servers) {
        const auto address = domain::InetAddress::parse(server.address);
        if (!address.has_value() || server.port == 0) {
            // A bootstrap server must be an IP literal with a nonzero port — a
            // config defect for this entry; the remaining servers can still
            // answer. Report the defect that was actually found rather than
            // both at once, and redact: the raw string may embed URI userinfo,
            // and the error message is handed to the caller just like the log
            // line is.
            const auto redacted = Utils::redacted_uri_credentials(server.address);
            const std::string_view defect =
                address.has_value() ? "must use a nonzero port" : "must be an IP literal with a nonzero port";
            SPDLOG_WARN(R"(Skipping bootstrap DNS server "{}": {})", redacted, defect);
            record_failure(server,
                           domain::DnsErrorInfo{domain::DnsError::CONFIG,
                                                fmt::format(R"(Bootstrap DNS server "{}" {})", redacted, defect)});
            continue;
        }

        try {
            const auto query = dns::build_query(host, dns::Util::type_to_record_type(kind));
            auto response = co_await detail::query_classic(*address, server.port, query);
            if (!response) {
                record_failure(server, std::move(response.error()));
                continue;
            }
            auto found = extract_addresses(*response, host, kind);
            if (!found) {
                // NXDOMAIN is authoritative for the name on this server only.
                record_failure(server, std::move(found.error()));
                continue;
            }
            if (!found->empty()) {
                result.addresses = std::move(*found);
                co_return result;
            }
            record_failure(server, {domain::DnsError::NODATA,
                                    fmt::format(R"(DNS lookup for "{}" returned no usable records)", host)});
        } catch (const DnsPacketException& error) {
            result.error = {domain::DnsError::INVALID_QUERY, error.what()};
            co_return result;
        } catch (const std::bad_alloc&) {
            throw;  // an allocation failure is never downgraded to a retryable error
        } catch (const DnsLookupException& error) {
            record_failure(server, {error.get_error(), error.what()});
        } catch (const std::exception& error) {
            record_failure(server, domain::DnsErrorInfo{domain::DnsError::PARSE,
                                                        fmt::format(R"(Failed to build or parse a query for "{}": {})",
                                                                    host, error.what())});
        }
    }

    co_return result;
}

/// Adapter so a kind can be resolved by a spawned group child.
[[nodiscard]] coro::Task<bool> resolve_into(std::string host, const domain::RecordKind kind,
                                            std::vector<domain::DnsServer> servers, KindResult* out) {
    *out = co_await resolve_kind(std::move(host), kind, std::move(servers));
    co_return !out->addresses.empty();
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
    std::string host, const std::optional<domain::AddressFamily> family, std::vector<domain::DnsServer> servers) {
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
        auto result = co_await resolve_kind(host, kinds.front(), std::move(servers));
        if (result.addresses.empty()) {
            co_return std::unexpected(with_host(host, std::move(result.error)));
        }
        co_return std::move(result.addresses);
    }

    // Once one family succeeds, collect the other for at most 50ms. A silent
    // family must not discard an address we can already connect to. The local
    // scope cancels and joins unfinished children; ancestor cancellation propagates.
    KindResult ipv4{host};
    KindResult ipv6{host};
    co_await coro::with_cancel_scope([&](coro::CancelScope& collection) -> coro::Task<void> {
        co_await coro::task_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            group.spawn(resolve_into(host, domain::RecordKind::A, servers, &ipv4));
            group.spawn(resolve_into(host, domain::RecordKind::AAAA, servers, &ipv6));
            auto first = co_await group.next<bool>();
            // task_group defects cancel this caller before delivery and are
            // rethrown after drain. A delivered Result therefore holds a value.
            if (!first)
                co_return;
            if (!first->value()) {
                [[maybe_unused]] auto second = co_await group.next<bool>();
                co_return;
            }
            // Bound just collection: the timeout detaches next() without
            // consuming a late result. Then cancel and structurally drain the
            // unresolved family through the enclosing collection scope.
            [[maybe_unused]] auto window = co_await coro::with_timeout(
                std::chrono::milliseconds(50),
                [&group]() -> coro::Task<void> { [[maybe_unused]] auto second = co_await group.next<bool>(); });
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
