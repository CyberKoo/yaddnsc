//
// Shared connection-policy construction for the TLS-based DNS resolvers
// (DoH / DoT).  One place builds the Transport::Options + TlsOptions pair
// and the TlsStream, so each resolver constructs the policy exactly once.
//

#ifndef YADDNSC_DNS_RESOLVER_TLS_OPTIONS_HPP
#define YADDNSC_DNS_RESOLVER_TLS_OPTIONS_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "domain/config/dns_config.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"
#include "infrastructure/network/transport/tls_stream.h"

namespace DNS::Resolver {
/// Connection-establishment budget shared by the TLS-based resolvers.
inline constexpr auto TLS_CONNECT_TIMEOUT = std::chrono::seconds{1};

/// Connection + TLS options for a TLS-based DNS connection (DoH / DoT).
/// @param bootstrap   Bootstrap DNS servers used to resolve the endpoint host.
/// @param alpn_proto  ALPN protocol identifier (RFC 7301); must outlive the
///                    TlsStream construction (the stream copies the bytes).
[[nodiscard]] inline std::pair<Transport::Options, Transport::TlsOptions> make_tls_options(
    std::vector<Config::DnsServer> bootstrap, const std::span<const unsigned char> alpn_proto) {
    Transport::Options conn;
    conn.connect_timeout = TLS_CONNECT_TIMEOUT;
    conn.bootstrap_dns = std::move(bootstrap);
    Transport::TlsOptions tls;
    tls.alpn_proto = alpn_proto;
    return {std::move(conn), std::move(tls)};
}

/// Open a TLS stream to a DNS resolver endpoint, constructing the connection
/// policy exactly once (single make_tls_options call).
[[nodiscard]] inline std::unique_ptr<Transport::Stream> make_tls_stream(
    const std::string& host, const std::uint16_t port, const std::vector<Config::DnsServer>& bootstrap,
    const std::span<const unsigned char> alpn_proto) {
    auto [conn, tls] = make_tls_options(bootstrap, alpn_proto);
    return std::make_unique<Transport::TlsStream>(host, port, std::move(conn), std::move(tls));
}
}  // namespace DNS::Resolver

#endif  // YADDNSC_DNS_RESOLVER_TLS_OPTIONS_HPP
