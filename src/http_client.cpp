//
// Created by Kotarou on 2022/4/5.
//
#include "http_client.h"

#include <filesystem>
#include <optional>

#include <sys/socket.h>

#include "fmt.h"

#include <httplib.h>
#include <spdlog/spdlog.h>

#include "http_types.h"
#include "uri.h"
#include "ip_util.h"
#include "version.h"

namespace {
    std::optional<std::string> get_system_ca_path() {
        static const std::optional<std::string> ca_path = []() -> std::optional<std::string> {
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
            return std::nullopt;
        }();

        return ca_path;
    }
}

httplib::Client HttpClient::detail::connect(const Uri &uri, address_family family, std::string_view nif_name) {
    SPDLOG_DEBUG("Connecting to {}", uri.get_host());
    auto client = httplib::Client(fmt::format("{}://{}:{}", uri.get_schema(), uri.get_host(), uri.get_port()));

    // if is https
    if (uri.get_schema() == "https") {
        if (const auto ca_path = get_system_ca_path()) {
            client.set_ca_cert_path(*ca_path);
            client.enable_server_certificate_verification(true);
        }
    }

    // set outbound interface
    if (!nif_name.empty()) {
        client.set_interface(nif_name.data());
    }

    // set address family
    client.set_address_family(IPUtil::ip2af(family));
    client.set_connection_timeout(std::chrono::seconds(5));
    client.set_read_timeout(std::chrono::seconds(5));
    client.set_follow_location(true);
    client.set_default_headers({{"User-Agent", yaddnsc::get_full_version()}});

    return client;
}

httplib::Result HttpClient::get(const Uri &uri, address_family family, std::string_view nif_name) {
    auto client = detail::connect(uri, family, nif_name);
    return client.Get(detail::build_request(uri));
}

httplib::Result HttpClient::post(const Uri &uri, const param_type &parameters, address_family family,
                                 std::string_view nif_name) {
    SPDLOG_TRACE("POST to URI {}", uri.get_raw_uri());

    auto client = detail::connect(uri, family, nif_name);
    return client.Post(detail::build_request(uri), parameters);
}

httplib::Result HttpClient::put(const Uri &uri, const param_type &parameters, address_family family,
                                std::string_view nif_name) {
    SPDLOG_TRACE("PUT to URI {}", uri.get_raw_uri());

    auto client = detail::connect(uri, family, nif_name);
    return client.Put(detail::build_request(uri), parameters);
}

httplib::Result HttpClient::patch(const Uri &uri, const param_type &parameters, address_family family,
                                  std::string_view nif_name) {
    SPDLOG_TRACE("PATCH to URI {}", uri.get_raw_uri());

    auto client = detail::connect(uri, family, nif_name);
    return client.Patch(detail::build_request(uri), parameters);
}

httplib::Result HttpClient::del(const Uri &uri, const param_type &parameters, address_family family,
                                std::string_view nif_name) {
    SPDLOG_TRACE("DELETE to URI {}", uri.get_raw_uri());

    auto client = detail::connect(uri, family, nif_name);
    return client.Delete(detail::build_request(uri), parameters);
}

std::string HttpClient::detail::build_request(const Uri &uri) {
    if (uri.get_query_string().empty()) {
        return std::string(uri.get_path());
    }

    return fmt::format("{}?{}", uri.get_path(), uri.get_query_string());
}

httplib::Result HttpClient::send(const http_request &req, address_family family, std::string_view nif_name) {
    const auto uri = Uri::parse(req.url);
    auto client = detail::connect(uri, family, nif_name);
    httplib::Headers headers{req.header.begin(), req.header.end()};
    const auto path = detail::build_request(uri);

    auto with_body = [&](auto method) -> httplib::Result {
        if (auto *body = std::get_if<http_param_type>(&req.body)) {
            return method(path.c_str(), std::move(headers), *body);
        }

        return method(path.c_str(), std::move(headers),
                      std::get<std::string>(req.body), req.content_type.c_str());
    };

    switch (req.request_method) {
        case http_method_type::GET:
            return client.Get(path, headers);
        case http_method_type::POST:
            return with_body([&]<typename... T>(T &&... args) { return client.Post(std::forward<T>(args)...); });
        case http_method_type::PUT:
            return with_body([&]<typename... T>(T &&... args) { return client.Put(std::forward<T>(args)...); });
        case http_method_type::PATCH:
            return with_body([&]<typename... T>(T &&... args) { return client.Patch(std::forward<T>(args)...); });
        case http_method_type::DEL:
            return client.Delete(path, headers);
        default:
            return client.Get(path, headers);
    }
}
