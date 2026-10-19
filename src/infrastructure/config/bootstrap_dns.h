#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_BOOTSTRAP_DNS_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_BOOTSTRAP_DNS_H

#include <string>
#include <string_view>

namespace Config {

/// The conclusion the bootstrap_dns field yields, consumed by both static
/// validation and normalization.
///
/// Bootstrap DNS is what resolves hostnames, so the field must name an IP
/// literal, and the DTO has no place to carry a scheme, port, path or query:
/// accepting one would drop it. Both answers therefore come from one owner so
/// validation and normalization cannot drift apart.
struct BootstrapDns {
    /// The address downstream consumers dial: the bare IP literal without the
    /// RFC 3986 IP-literal brackets, or the field verbatim when it names no IP
    /// literal at all. A verbatim value is reported by validation, never used.
    std::string address;

    /// Whether @a address is the canonical form of an accepted value: the field
    /// holds an IP literal and carries no scheme, port, path or query.
    bool valid{false};
};

/// Resolve the raw bootstrap_dns field. The empty field is a valid value —
/// it means "fall back to /etc/resolv.conf" — and yields a valid, empty address.
///
/// Pure function of its argument: safe to call from validation and
/// normalization independently, in either order.
[[nodiscard]] BootstrapDns resolve_bootstrap_dns(std::string_view field);

}  // namespace Config

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_BOOTSTRAP_DNS_H
