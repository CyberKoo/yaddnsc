//
// net — construction-time options for transport streams.
//
// Deliberately small:
//   - no connect/read/write timeouts: a deadline is the caller's cancel scope
//     (`with_timeout(...)`), never an I/O parameter;
//   - no bootstrap_dns: HTTP receives an injected hostname resolver;
//   - no address_family: the connection target is an already-resolved
//     InetAddress, so the family is carried by the value itself.
// Nothing is silently accepted and ignored.
//

#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_OPTIONS_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_OPTIONS_H

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

/// Per-stream TLS options, accepted exclusively by TlsStream.
///
/// Trust policy lives in TlsTrustOptions instead: it is consumed once by
/// TlsContext::create() and shared through the built context, so a field set
/// here can never be silently ignored.
struct TlsOptions {
    /// Hostname used for SNI and certificate verification. Layers that know the
    /// origin name (the HTTP clients, the DoT resolver) default this to the
    /// target host when it is a name — never an IP literal, which RFC 6066 §3
    /// forbids in SNI; an explicitly empty string suppresses that default.
    /// With no name, the connection target itself is the identity: verification
    /// against the IP literal and no SNI.
    std::optional<std::string> sni_hostname{};

    /// ALPN protocol bytes, e.g. {2, 'h', '2'}. Copied at construction.
    std::span<const unsigned char> alpn_proto{};
};

/// Trust policy, consumed exactly once by TlsContext::create().
struct TlsTrustOptions {
    /// Verify the peer certificate (fail-closed). Default: true.
    bool verify_peer{true};

    /// Explicit CA bundle path. Default: Utils::Cert::discover_ca_bundle().
    /// Without a discovered bundle, eagerly snapshot the default OpenSSL hashed
    /// cert directory. Missing/empty or malformed indexed sources fail closed;
    /// no lazy trust-directory lookup is installed.
    std::optional<std::string> ca_bundle{};
};

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_OPTIONS_H
