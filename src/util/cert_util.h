//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_UTIL_CERT_UTIL_H
#define YADDNSC_UTIL_CERT_UTIL_H

#include <optional>
#include <string>

namespace Utils::Cert {
    /// Four-tier CA bundle discovery:
    ///   1. SSL_CERT_FILE environment variable
    ///   2. Local ./ca.pem (dev/test override)
    ///   3. OpenSSL default file path (X509_get_default_cert_file)
    ///   4. Well-known hardcoded system paths
    ///
    /// The result is cached after the first invocation.
    ///
    /// @return  Path to a CA bundle file, or std::nullopt.
    [[nodiscard]] std::optional<std::string> discover_ca_bundle();

    /// Search well-known system locations for a CA certificate bundle file.
    ///
    /// Returns the path to the first found bundle, or std::nullopt if none were
    /// found. The result is cached after the first invocation.
    ///
    /// @note  Consider using discover_ca_bundle() instead, which additionally
    ///        checks SSL_CERT_FILE, ./ca.pem, and the OpenSSL default path.
    ///
    /// @return  Path to a CA bundle file, or std::nullopt.
    [[nodiscard]] std::optional<std::string> get_system_ca_path();
} // namespace Utils::Cert

#endif // YADDNSC_UTIL_CERT_UTIL_H
