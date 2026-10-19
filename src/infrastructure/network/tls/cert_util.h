#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_TLS_CERT_UTIL_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_TLS_CERT_UTIL_H

#include <optional>
#include <string>

namespace Utils::Cert {
/// Three-tier CA bundle discovery:
///   1. SSL_CERT_FILE environment variable (explicit override)
///   2. OpenSSL default file path (X509_get_default_cert_file)
///   3. Well-known hardcoded system paths
///
/// The result is cached after the first invocation.
///
/// @return  Path to a CA bundle file, or std::nullopt.
[[nodiscard]] std::optional<std::string> discover_ca_bundle();
}  // namespace Utils::Cert

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TLS_CERT_UTIL_H
