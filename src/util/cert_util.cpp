//
// Created by Kotarou on 2026/6/30.
//

#include "cert_util.h"

#include <ranges>
#include <filesystem>
#include <string_view>

#include <spdlog/spdlog.h>

namespace CertUtil {
    std::optional<std::string> get_system_ca_path() {
        static const std::optional<std::string> system_ca_path = []() -> std::optional<std::string> {
            constexpr std::string_view search_paths[]{
                // Local CA file
                "./ca.pem",
                // Debian/Ubuntu/Gentoo etc.
                "/etc/ssl/certs/ca-certificates.crt",
                // CentOS/RHEL 7
                "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                // OpenSUSE
                "/etc/ssl/ca-bundle.pem",
                // macOS via Homebrew
                "/usr/local/etc/openssl/cert.pem",
                // macOS via Homebrew (Apple Silicon)
                "/opt/homebrew/etc/openssl/cert.pem",
                // Fedora/RHEL 6
                "/etc/pki/tls/certs/ca-bundle.crt",
                // OpenELEC
                "/etc/pki/tls/cacert.pem",
                // OpenWRT
                "/etc/ssl/cert.pem",
                // FreeBSD (ca_root_nss package)
                "/usr/local/share/certs/ca-root-nss.crt",
                // FreeBSD/OpenSSL
                "/etc/ssl/cert.pem",
                // OpenBSD
                "/etc/ssl/cert.pem",
                // NetBSD (pkgsrc)
                "/etc/openssl/certs/ca-certificates.crt",
                // NetBSD/OpenSSL
                "/etc/openssl/cert.pem",
            };

            SPDLOG_DEBUG("Looking for CA bundle...");

            const auto it = std::ranges::find_if(search_paths, [](std::string_view p) {
                return std::filesystem::exists(p) && !std::filesystem::is_directory(p);
            });

            if (it != std::end(search_paths)) {
                SPDLOG_DEBUG("Found CA bundle at {}", *it);
                return std::string(*it);
            }

            SPDLOG_WARN("CA bundle not found, server certificate verification will be disabled.");
            return std::nullopt;
        }();

        return system_ca_path;
    }
} // namespace CertUtil
