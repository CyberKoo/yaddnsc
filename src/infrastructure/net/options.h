//
// net — construction-time options for transport streams.
//
// Deliberately small. Compared with the legacy Transport::Options:
//   - connect_timeout / read_timeout / write_timeout are gone: a deadline is the
//     caller's cancel scope (`with_timeout(...)`), never an I/O parameter;
//   - bootstrap_dns is gone: resolving a hostname needs the resolver port, which
//     arrives in stage 2b;
//   - address_family is gone: the connection target is an already-resolved
//     InetAddress, so the family is carried by the value itself.
// Nothing is silently accepted and ignored.
//

#ifndef YADDNSC_NET_OPTIONS_H
#define YADDNSC_NET_OPTIONS_H

#include <optional>
#include <span>
#include <string>

namespace net {

/// Connection-level options shared by TcpStream and TlsStream.
struct ConnectOptions {
    /// Outbound interface name (SO_BINDTODEVICE). Linux only; on platforms
    /// without it the binding is best-effort and skipped.
    std::optional<std::string> interface{};
};

/// TLS-only options, accepted exclusively by TlsStream.
struct TlsOptions {
    /// Hostname used for SNI and certificate verification. Default: the
    /// connection target itself, i.e. verification against the IP literal and no
    /// SNI (RFC 6066 §3 forbids an IP literal in SNI).
    std::optional<std::string> sni_hostname{};

    /// ALPN protocol bytes, e.g. {2, 'h', '2'}. Copied at construction.
    std::span<const unsigned char> alpn_proto{};

    /// Verify the peer certificate (fail-closed). Default: true.
    bool verify_peer{true};

    /// Explicit CA bundle path. Default: `Utils::Cert::discover_ca_bundle()`,
    /// then OpenSSL's default verify paths.
    std::optional<std::string> ca_bundle{};
};

}  // namespace net

#endif  // YADDNSC_NET_OPTIONS_H
