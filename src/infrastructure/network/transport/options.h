//
// Aggregated construction-time options for Transport streams.
//
// Split by layer: Options carries connection-level settings shared by every
// stream (TCP and TLS alike); TlsOptions carries TLS-only settings and is
// accepted only by TLS streams. Nothing is silently ignored.
//

#ifndef YADDNSC_TRANSPORT_OPTIONS_PUBLIC_H
#define YADDNSC_TRANSPORT_OPTIONS_PUBLIC_H

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "domain/config/dns_config.h"
#include "domain/network/address_family.h"

namespace Transport {

/// Connection-level options shared by TCP and TLS streams.
struct Options {
    /// Budget for one ensure_connected() call. Name resolution, address
    /// attempts, the TCP handshake, and (for TLS) the TLS handshake share it.
    /// EINTR and WANT_* retries do not extend it.
    std::chrono::milliseconds connect_timeout{5000};

    /// Budget for one read_some() call, or for the whole read_exact() call.
    /// Partial reads, EINTR, and TLS WANT_* retries do not extend it.
    /// Once it is spent the call does not recv, even if the socket is
    /// already readable. A later read on the same stream starts a new budget.
    /// Bytes OpenSSL has already decrypted are not a new socket read.
    std::chrono::milliseconds read_timeout{5000};

    /// Budget for one send_all() call. Partial writes, EINTR, and TLS
    /// WANT_* retries do not extend it. Once it is spent the call does not
    /// send, even if the socket is already writable.
    std::chrono::milliseconds write_timeout{5000};

    /// Outbound interface name (SO_BINDTODEVICE, Linux only; ignored
    /// elsewhere).
    std::optional<std::string> interface{};

    /// Restrict name resolution to this address family.
    std::optional<AddressFamily> address_family{};

    /// Bootstrap DNS servers (IP literals) used to resolve hostname targets
    /// via the built-in classic resolver. getaddrinfo/NSS is never used:
    /// /etc/hosts and friends do not apply. Empty: hostname targets fail
    /// fast with an actionable error.
    std::vector<Config::DnsServer> bootstrap_dns{};
};

/// TLS-only options. Accepted exclusively by TlsStream / create_tls —
/// a plain-TCP code path never sees these.
struct TlsOptions {
    /// Override the hostname used for SNI and certificate verification.
    /// Default: the connection target host.
    std::optional<std::string> sni_hostname{};

    /// ALPN protocol bytes (e.g. {2, 'h', '2'}). The stream copies the
    /// bytes at construction.
    std::span<const unsigned char> alpn_proto{};

    /// Verify the peer certificate (fail-closed). Default: true.
    bool verify_peer{true};

    /// Explicit CA bundle path. Default: automatic discovery
    /// (Utils::Cert::discover_ca_bundle), then OpenSSL default paths.
    std::optional<std::string> ca_bundle{};
};

}  // namespace Transport

#endif  // YADDNSC_TRANSPORT_OPTIONS_PUBLIC_H
