#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_TLS_CERT_UTIL_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_TLS_CERT_UTIL_H

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/// Explicit SSL_CERT_DIR entries in POSIX colon-separated order, or nullopt
/// when the variable is unset or empty. The environment is read on each call.
[[nodiscard]] inline std::optional<std::vector<std::string>> discover_ca_directories() {
    const auto* env = std::getenv("SSL_CERT_DIR");
    if (env == nullptr || *env == '\0') {
        return std::nullopt;
    }

    std::vector<std::string> directories;
    std::string_view remaining(env);
    for (;;) {
        const auto separator = remaining.find(':');
        const auto directory = remaining.substr(0, separator);
        if (!directory.empty()) {
            directories.emplace_back(directory);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    return directories;
}
}  // namespace Utils::Cert

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TLS_CERT_UTIL_H
