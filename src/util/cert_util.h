//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_UTIL_CERT_UTIL_H
#define YADDNSC_UTIL_CERT_UTIL_H

#include <optional>
#include <string>

namespace CertUtil {
    // Search well-known system locations for a CA certificate bundle file.
    // Returns the path to the first found bundle, or std::nullopt if none were
    // found. The result is cached after the first invocation.
    std::optional<std::string> get_system_ca_path();
} // namespace CertUtil

#endif // YADDNSC_UTIL_CERT_UTIL_H
