//
// Bootstrap name resolution for the transport layer (no getaddrinfo/NSS).
//

#include "bootstrap.h"

#include <cstdint>
#include <utility>

#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>

#include "domain/dns/record_kind.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/classic/classic_tcp.h"
#include "infrastructure/dns/classic/classic_udp.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/network/socket_addr.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

namespace DNS {

namespace {

/// Record kinds to query, in order, for the requested address family.
[[nodiscard]] std::vector<RecordKind> kinds_for(const std::optional<AddressFamily> family) {
    switch (family.value_or(AddressFamily::UNSPECIFIED)) {
        case AddressFamily::IPV4:
            return {RecordKind::A};
        case AddressFamily::IPV6:
            return {RecordKind::AAAA};
        default:
            return {RecordKind::A, RecordKind::AAAA};
    }
}

[[nodiscard]] bool is_truncated(const std::span<const std::uint8_t> response) {
    return response.size() >= DNS::HEADER_SIZE && (response[2] & 0x02) != 0;
}

/// Extract addresses of the queried kind from a raw response packet.
/// NXDOMAIN is authoritative and reported as an error immediately.
/// Throws DnsLookupException on malformed packets — the caller translates.
[[nodiscard]] std::expected<std::vector<InetAddress>, DnsErrorInfo> extract_addresses(
    const std::vector<std::uint8_t>& response, const std::string& host, const RecordKind kind) {
    const auto expected_type = kind == RecordKind::A ? static_cast<std::uint16_t>(RecordType::A)
                                                     : static_cast<std::uint16_t>(RecordType::AAAA);

    const auto parsed = RecordParser::parse_response(response, host);

    if (parsed.rcode == Rcode::NXDOMAIN) {
        return std::unexpected(
            DnsErrorInfo{DnsError::NX_DOMAIN, fmt::format(R"(Domain "{}" does not exist (NXDOMAIN))", host)});
    }

    std::vector<InetAddress> addresses;
    for (const auto& rr : parsed.answers) {
        if (rr.type != expected_type) {
            continue;
        }
        if (auto addr = InetAddress::from_bytes(rr.rdata)) {
            addresses.push_back(*addr);
        }
    }
    return addresses;
}

}  // namespace

std::expected<std::vector<InetAddress>, DnsErrorInfo> resolve_bootstrap(
    const std::string& host, const std::optional<AddressFamily> family,
    const std::span<const Config::DnsServer> servers, const std::chrono::steady_clock::time_point deadline,
    const Utils::CancellationToken& token) {
    const auto kinds = kinds_for(family);
    DnsErrorInfo last_error{DnsError::NODATA,
                            fmt::format(R"(No address records found for "{}" via bootstrap DNS)", host)};

    for (const auto& server : servers) {
        if (token.is_triggered()) {
            return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "Bootstrap DNS query cancelled"});
        }

        auto parsed = InetAddress::parse(server.address);
        auto endpoint = parsed ? SocketAddr::from_inet(*parsed, server.port) : std::nullopt;
        if (!endpoint) {
            last_error = DnsErrorInfo{DnsError::CONFIG,
                                      fmt::format(R"(Bootstrap DNS server "{}" is not an IP address)", server.address)};
            continue;
        }

        std::vector<InetAddress> addresses;

        for (const auto kind : kinds) {
            if (token.is_triggered()) {
                return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "Bootstrap DNS query cancelled"});
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::unexpected(DnsErrorInfo{
                    DnsError::RETRY, fmt::format(R"(Bootstrap DNS deadline exceeded while resolving "{}")", host)});
            }

            // Packet construction can throw. The failure is recorded below
            // and the next server is tried; a catch does not decide control flow.
            bool server_failed = false;
            try {
                const auto query = DNS::build_query(host, DNS::Util::type_to_record_type(kind));
                auto response = DNS::exchange_udp(*endpoint, query, deadline, token, 0);
                if (!response) {
                    if (response.error().code == DnsError::CANCELLED) {
                        return std::unexpected(std::move(response.error()));
                    }
                    last_error = std::move(response.error());
                    server_failed = true;
                } else if (auto valid = DNS::Validator::validate_response(query, *response); !valid) {
                    // A malformed UDP packet is not retried. TCP fallback is
                    // only for a response the validator has already accepted.
                    last_error = std::move(valid.error());
                    server_failed = true;
                } else {
                    if (is_truncated(*response)) {
                        SPDLOG_DEBUG(R"(Bootstrap DNS response for "{}" was truncated; retrying over TCP)", host);
                        response = DNS::exchange_tcp(*endpoint, query, deadline, token, 0);
                        if (!response) {
                            if (response.error().code == DnsError::CANCELLED) {
                                return std::unexpected(std::move(response.error()));
                            }
                            last_error = std::move(response.error());
                            server_failed = true;
                        } else if (auto tcp_valid = DNS::Validator::validate_response(query, *response); !tcp_valid) {
                            last_error = std::move(tcp_valid.error());
                            server_failed = true;
                        }
                    }
                    if (!server_failed) {
                        auto found = extract_addresses(*response, host, kind);
                        if (!found) {
                            // NXDOMAIN is authoritative for the NAME: no other record
                            // type exists either, so stop querying this server — but
                            // keep any addresses already collected (split-horizon
                            // servers sometimes answer one kind and NXDOMAIN the
                            // other).
                            last_error = std::move(found.error());
                            server_failed = true;
                        } else {
                            addresses.insert(addresses.end(), found->begin(), found->end());
                        }
                    }
                }
            } catch (const std::exception& e) {
                last_error =
                    DnsErrorInfo{DnsError::PARSE,
                                 fmt::format(R"(Failed to parse bootstrap DNS response for "{}": {})", host, e.what())};
                server_failed = true;
            }

            if (server_failed) {
                break;  // this server cannot answer the remaining kinds
            }
        }

        if (!addresses.empty()) {
            SPDLOG_DEBUG(R"(Resolved "{}" to {} address(es) via bootstrap DNS {}:{})", host, addresses.size(),
                         server.address, server.port);
            return addresses;
        }
    }

    return std::unexpected(std::move(last_error));
}

}  // namespace DNS
