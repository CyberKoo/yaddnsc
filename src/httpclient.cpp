//
// Created by Kotarou on 2022/4/5.
//
#include "httpclient.h"

#include <filesystem>

#include "fmt.h"

#include <httplib.h>
#include <spdlog/spdlog.h>

#include "uri.h"
#include "version.h"

std::string get_system_ca_path();

httplib::Client HttpClient::connect(const Uri &uri, int family, const char *nif_name) {
    SPDLOG_DEBUG("Connecting to {}", uri.get_host());
    auto client = httplib::Client(fmt::format("{}://{}:{}", uri.get_schema(), uri.get_host(), uri.get_port()));

    // if is https
    if (uri.get_schema() == "https") {
        auto ca_path = get_system_ca_path();

        if (!ca_path.empty()) {
            client.set_ca_cert_path(ca_path);
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
    client.set_default_headers({{"User-Agent", yaddnsc::get_full_version()}});

    return client;
}

httplib::Result HttpClient::get(const Uri &uri, int family, const char *nif_name) {
    auto client = HttpClient::connect(uri, family, nif_name);
    return client.Get(build_request(uri));
}

httplib::Result HttpClient::post(const Uri &uri, const param_type &parameters, int family, const char *nif_name) {
    SPDLOG_TRACE("POST to URI {}", uri.get_raw_uri());

    auto client = HttpClient::connect(uri, family, nif_name);
    return client.Post(build_request(uri), parameters);
}

httplib::Result HttpClient::put(const Uri &uri, const param_type &parameters, int family, const char *nif_name) {
    SPDLOG_TRACE("PUT to URI {}", uri.get_raw_uri());

    auto client = HttpClient::connect(uri, family, nif_name);
    return client.Put(build_request(uri), parameters);
}

std::string HttpClient::build_request(const Uri &uri) {
    if (uri.get_query_string().empty()) {
        return std::string(uri.get_path());
    }

    return fmt::format("{}?{}", uri.get_path(), uri.get_query_string());
}

std::string get_system_ca_path() {
    static const std::string ca_path = [] {
        constexpr std::string_view search_paths[]{
            "./ca.pem",
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
            "/etc/ssl/ca-bundle.pem",
            "/usr/local/etc/openssl/cert.pem",
            "/opt/homebrew/etc/openssl/cert.pem",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/pki/tls/cacert.pem",
            "/etc/ssl/cert.pem",
        };

        SPDLOG_DEBUG("Looking for CA bundle...");

        const auto it = std::ranges::find_if(search_paths, [](std::string_view p) {
            return std::filesystem::exists(p) && !std::filesystem::is_directory(p);
        });

        if (it != std::end(search_paths)) {
            SPDLOG_DEBUG("Found CA bundle at {}", *it);
            return std::string(*it);
        }

        SPDLOG_INFO("CA bundle not found, server certificate verification will be disabled.");
        return std::string{};
    }();

    return ca_path;
}
