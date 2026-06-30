//
// Created by Kotarou on 2026/6/30.
//

#include "cert_util.h"

#include <filesystem>

#include <spdlog/spdlog.h>

std::string_view get_system_ca_path() {
    static std::string_view ca_path = []() -> std::string_view {
        constexpr std::string_view SEARCH_PATH[]{
            "./ca.pem",                                             // Local CA file
            "/etc/ssl/certs/ca-certificates.crt",                   // Debian/Ubuntu/Gentoo etc.
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",    // CentOS/RHEL 7
            "/etc/ssl/ca-bundle.pem",                               // OpenSUSE
            "/usr/local/etc/openssl/cert.pem",                      // MacOS via Homebrew
            "/opt/homebrew/etc/openssl/cert.pem",                   // MacOS via Homebrew(M1 and above)
            "/etc/pki/tls/certs/ca-bundle.crt",                     // Fedora/RHEL 6
            "/etc/pki/tls/cacert.pem",                              // OpenELEC
            "/etc/ssl/cert.pem",                                    // OpenWRT
        };

        SPDLOG_DEBUG("Looking for CA bundle...");
        for (const auto &search: SEARCH_PATH) {
            if (std::filesystem::exists(search) && !std::filesystem::is_directory(search)) {
                SPDLOG_DEBUG("Found CA bundle at {}", search);
                return search;
            }
        }

        SPDLOG_INFO("CA bundle not found, server certificate verification will be disabled.");

        return "";
    }();

    return ca_path;
}
