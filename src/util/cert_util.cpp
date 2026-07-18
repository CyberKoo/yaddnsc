//
// Created by Kotarou on 2026/7/18.
//

#include "util/cert_util.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <openssl/x509.h>

#include <spdlog/spdlog.h>

namespace Utils::Cert {

// ── Cached hardcoded search paths (tier 4) ──────────────────────────────

namespace {
    [[nodiscard]] const std::optional<std::string> &get_hardcoded_paths() {
        static const std::optional<std::string> path = []() -> std::optional<std::string> {
            static constexpr std::string_view SEARCH_PATHS[]{
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

            for (const auto &p: SEARCH_PATHS) {
                if (std::filesystem::is_regular_file(p)) {
                    return std::string(p);
                }
            }

            return std::nullopt;
        }();

        return path;
    }
} // anonymous namespace

// ── get_system_ca_path ──────────────────────────────────────────────────

std::optional<std::string> get_system_ca_path() {
    static const std::optional<std::string> system_ca_path = []() -> std::optional<std::string> {
        SPDLOG_DEBUG("Looking for CA bundle...");

        try {
            const auto &hardcoded = get_hardcoded_paths();
            if (hardcoded) {
                SPDLOG_DEBUG("Found CA bundle at {}", *hardcoded);
                return *hardcoded;
            }
        } catch (const std::filesystem::filesystem_error &e) {
            SPDLOG_ERROR("Failed to search for CA bundle: {}", e.what());
            return std::nullopt;
        }

        SPDLOG_WARN("CA bundle not found, server certificate verification will be disabled.");
        return std::nullopt;
    }();

    return system_ca_path;
}

// ── discover_ca_bundle ──────────────────────────────────────────────────

std::optional<std::string> discover_ca_bundle() {
    static const std::optional<std::string> ca_bundle = []() -> std::optional<std::string> {
        // Tier 1: SSL_CERT_FILE environment variable
        if (const auto *env = std::getenv("SSL_CERT_FILE"); env != nullptr && *env != '\0') {
            if (std::filesystem::is_regular_file(env)) {
                SPDLOG_DEBUG("Found CA bundle via SSL_CERT_FILE: {}", env);
                return std::string(env);
            }
            SPDLOG_WARN("SSL_CERT_FILE points to non-existent file: {}", env);
        }

        // Tier 2: Local ./ca.pem (dev/test override)
        if (std::filesystem::is_regular_file("./ca.pem")) {
            SPDLOG_DEBUG("Found CA bundle at ./ca.pem");
            return "./ca.pem";
        }

        // Tier 3: OpenSSL default cert file path
        if (const auto *default_path = X509_get_default_cert_file(); default_path != nullptr && *default_path != '\0') {
            if (std::filesystem::is_regular_file(default_path)) {
                SPDLOG_DEBUG("Found CA bundle via OpenSSL default: {}", default_path);
                return std::string(default_path);
            }
        }

        // Tier 4: Hardcoded system paths
        const auto &hardcoded = get_hardcoded_paths();
        if (hardcoded) {
            SPDLOG_DEBUG("Found CA bundle at {}", *hardcoded);
            return *hardcoded;
        }

        SPDLOG_WARN("CA bundle not found; server certificate verification may be unavailable.");
        return std::nullopt;
    }();

    return ca_bundle;
}

} // namespace Utils::Cert
