//
// Created by Kotarou on 2022/4/5.
//
#include "httpclient.h"

#include <filesystem>
#include <httplib.h>
#include <spdlog/spdlog.h>

#include "uri.h"
#include "version.h"

std::string_view get_system_ca_path();

httplib::Client HttpClient::connect(const Uri &uri, int family, const char *nif_name) {
    SPDLOG_DEBUG("Connecting to {}", uri.get_host());
    auto client = httplib::Client(fmt::format("{}://{}:{}", uri.get_schema(), uri.get_host(), uri.get_port()));

    // if is https
    if (uri.get_schema() == "https") {
        auto ca_path = get_system_ca_path();

        if (!ca_path.empty()) {
            client.set_ca_cert_path(ca_path.data());
            client.enable_server_certificate_verification(true);
        }
    }

    // set outbound interface
    if (nif_name != nullptr) {
        client.set_interface(nif_name);
    }

    // set address family
    client.set_address_family(family);
    client.set_connection_timeout(std::chrono::seconds(5));
    client.set_read_timeout(std::chrono::seconds(5));
    client.set_follow_location(true);
    client.set_default_headers({{"User-Agent", get_full_version()}});

    return client;
}

httplib::Result HttpClient::get(const Uri &uri, int family, const char *nif_name) {
    auto client = HttpClient::connect(uri, family, nif_name);
    return client.Get(HttpClient::build_request(uri).c_str());
}

httplib::Result HttpClient::post(const Uri &uri, const param_type &parameters, int family, const char *nif_name) {
    SPDLOG_TRACE("Post to uri {}", uri.get_raw_uri());

    auto client = HttpClient::connect(uri, family, nif_name);
    return client.Post(HttpClient::build_request(uri).c_str(), parameters);
}

httplib::Result HttpClient::put(const Uri &uri, const param_type &parameters, int family, const char *nif_name) {
    SPDLOG_TRACE("Put to uri {}", uri.get_raw_uri());

    auto client = HttpClient::connect(uri, family, nif_name);
    return client.Put(HttpClient::build_request(uri).c_str(), parameters);
}

std::string HttpClient::build_request(const Uri &uri) {
    if (uri.get_query_string().empty()) {
        return uri.get_path().data();
    } else {
        return fmt::format("{}?{}", uri.get_path(), uri.get_query_string());
    }
}

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
