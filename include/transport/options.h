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

#include "address_family.h"

namespace Transport {

/// Connection-level options shared by TCP and TLS streams.
struct Options {
    /// Budget for connection establishment (connect + TLS handshake;
    /// name resolution itself is blocking and not covered by this).
    std::chrono::milliseconds connect_timeout{5000};

    /// Timeout for each poll() iteration while reading.
    std::chrono::milliseconds read_timeout{5000};

    /// Timeout for each poll() iteration while writing.
    std::chrono::milliseconds write_timeout{5000};

    /// Outbound interface name (SO_BINDTODEVICE, Linux only; ignored
    /// elsewhere).
    std::optional<std::string> interface{};

    /// Restrict name resolution to this address family.
    std::optional<AddressFamily> address_family{};
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

} // namespace Transport

#endif // YADDNSC_TRANSPORT_OPTIONS_PUBLIC_H
