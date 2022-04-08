//
// Created by Kotarou on 2022/4/5.
//
#include <filesystem>
#include <httplib.h>
#include <spdlog/spdlog.h>

#include "uri.h"
#include "httpclient.h"

std::unique_ptr<httplib::Client> HttpClient::connect(const Uri &uri, int family, const char *nif_name) {
    auto client = std::make_unique<httplib::Client>(
            fmt::format("{}://{}:{}", uri.get_schema(), uri.get_host(), uri.get_port()).data());

    // if is https
    if (uri.get_schema() == "https") {
        auto ca_path = get_system_ca_path();

        if (!ca_path.empty()) {
            client->set_ca_cert_path(ca_path.data());
            client->enable_server_certificate_verification(true);
        }
    }

    // set outbound interface
    if (nif_name != nullptr) {
        client->set_interface(nif_name);
    }

    // set address family
    client->set_address_family(family);

    client->set_connection_timeout(5);
    client->set_read_timeout(5, 1000);
    client->set_follow_location(true);
    client->set_default_headers({{"User-Agent", "Mozilla"}});

    return client;
}

httplib::Result HttpClient::get(const Uri &uri, int family, const char *nif_name) {
    auto client = HttpClient::connect(uri, family, nif_name);
    auto x = HttpClient::build_request(uri);
    auto response = client->Get(HttpClient::build_request(uri).c_str());

    return response;
}


httplib::Result HttpClient::post(const Uri &uri, const Params &parameters, int family, const char *nif_name) {
    SPDLOG_TRACE("Post to uri {}", uri.get_raw_uri());

    auto client = HttpClient::connect(uri, family, nif_name);
    auto response = client->Post(HttpClient::build_request(uri).c_str(), parameters);

    return response;
}

httplib::Result HttpClient::put(const Uri &uri, const Params &parameters, int family, const char *nif_name) {
    SPDLOG_TRACE("Put to uri {}", uri.get_raw_uri());

    auto client = HttpClient::connect(uri, family, nif_name);
    auto response = client->Put(HttpClient::build_request(uri).c_str(), parameters);

    return response;
}

std::string HttpClient::build_request(const Uri &uri) {
    if (uri.get_query_string().empty()) {
        return uri.get_path().data();
    } else {
        return fmt::format("{}?{}", uri.get_path(), uri.get_query_string());
    }
}

std::string_view HttpClient::get_system_ca_path() {
    static std::string_view path = []() -> std::string_view {
        constexpr std::string_view SEARCH_PATH[]{
                "/etc/ssl/certs/ca-certificates.crt",                // Debian/Ubuntu/Gentoo etc.
                "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora/RHEL 6
                "/etc/ssl/ca-bundle.pem",                            // OpenSUSE
                "/etc/pki/tls/cacert.pem",                           // OpenELEC
                "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS/RHEL 7
                "/usr/local/etc/openssl/cert.pem",                   // MacOS via Homebrew
                "./ca.pem"                                           // Load local
        };

        SPDLOG_TRACE("Looking for CA bundle");
        for (const auto &search: SEARCH_PATH) {
            if (std::filesystem::exists(search)) {
                SPDLOG_INFO("Found CA bundle at {}", search);
                return search;
            }
        }

        return "";
    }();

    return path;
}
