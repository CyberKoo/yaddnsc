//
// Created by Kotarou on 2022/4/5.
//
#include "http_client.h"

#include <sys/socket.h>

#include <filesystem>
#include <optional>
#include <utility>

#include <httplib.h>
#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "http_types.h"
#include "ip_util.h"
#include "uri.h"
#include "version.h"

namespace {
    std::optional<std::string> get_system_ca_path() {
        static const std::optional<std::string> ca_path = []() -> std::optional<std::string> {
            constexpr std::string_view search_paths[]{
                "./ca.pem",
                "/etc/ssl/certs/ca-certificates.crt",
                "/etc/ssl/cert.pem",
                "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                "/etc/ssl/ca-bundle.pem",
                "/usr/local/etc/ssl/cert.pem",
                "/opt/homebrew/etc/openssl/cert.pem",
                "/etc/pki/tls/certs/ca-bundle.crt",
                "/etc/pki/tls/cacert.pem",
                "/usr/local/share/certs/ca-root-nss.crt", // OPNsense
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

        return ca_path;
    }

    httplib::Client connect(const Uri &uri, address_family family, const std::optional<std::string> &nif_name) {
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
        if (nif_name.has_value() && !nif_name->empty()) {
            client.set_interface(nif_name->c_str());
        }

        // set address family
        client.set_address_family(IPUtil::to_socket_type(family));
        client.set_connection_timeout(std::chrono::seconds(5));
        client.set_read_timeout(std::chrono::seconds(5));
        client.set_follow_location(true);
        client.set_default_headers({{"User-Agent", yaddnsc::get_full_version()}});

        return client;
    }

    std::string build_request(const Uri &uri) {
        if (uri.get_query_string().empty()) {
            return std::string(uri.get_path());
        }

        return fmt::format("{}?{}", uri.get_path(), uri.get_query_string());
    }

    // -----------------------------------------------------------------------
    // Dispatch: http_method_type -> httplib callable
    // -----------------------------------------------------------------------

    using Invoker = httplib::Result (*)(httplib::Client &, const char *, const httplib::Headers &,
                                        const std::optional<std::string> &, const char *);

    constexpr Invoker INVOKERS[]{
        [](httplib::Client &c, const char *p, const httplib::Headers &h, const std::optional<std::string> &,
           const char *) -> httplib::Result {
            return c.Get(p, h);
        },
        [](httplib::Client &c, const char *p, const httplib::Headers &h, const std::optional<std::string> &b,
           const char *ct) -> httplib::Result {
            return c.Post(p, h, b.value_or(""), ct);
        },
        [](httplib::Client &c, const char *p, const httplib::Headers &h, const std::optional<std::string> &b,
           const char *ct) -> httplib::Result {
            return c.Put(p, h, b.value_or(""), ct);
        },
        [](httplib::Client &c, const char *p, const httplib::Headers &h, const std::optional<std::string> &,
           const char *) -> httplib::Result {
            return c.Delete(p, h);
        },
        [](httplib::Client &c, const char *p, const httplib::Headers &h, const std::optional<std::string> &b,
           const char *ct) -> httplib::Result {
            return c.Patch(p, h, b.value_or(""), ct);
        },
        [](httplib::Client &c, const char *p, const httplib::Headers &h, const std::optional<std::string> &,
           const char *) -> httplib::Result {
            return c.Head(p, h);
        },
        [](httplib::Client &c, const char *p, const httplib::Headers &h, const std::optional<std::string> &,
           const char *) -> httplib::Result {
            return c.Options(p, h);
        },
    };
}

// ---------------------------------------------------------------------------
// HttpClient (static)
// ---------------------------------------------------------------------------

std::string HttpClient::params_to_query_string(const http_param_type &params) {
    auto encoded = params | std::views::transform([](const auto &p) {
        return fmt::format("{}={}",
                           httplib::encode_query_component(p.first),
                           httplib::encode_query_component(p.second)
        );
    });
    return fmt::format("{}", fmt::join(encoded, "&"));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HttplibHttpClient::HttplibHttpClient(address_family af, std::optional<std::string> interface)
    : af_(af), interface_(std::move(interface)) {
}

// ---------------------------------------------------------------------------
// HttpClient
// ---------------------------------------------------------------------------

void HttplibHttpClient::set_address_family(address_family af) {
    af_ = af;
}

HttpResponse HttplibHttpClient::send(const http_request &req) const {
    const auto uri = Uri::parse(req.url);
    auto client = connect(uri, af_, interface_);
    httplib::Headers headers{req.header.begin(), req.header.end()};
    const auto path = build_request(uri);

    const auto result = INVOKERS[std::to_underlying(req.request_method)](
        client, path.c_str(), headers, req.body, req.content_type.data()
    );

    if (!result) {
        return std::unexpected(httplib::to_string(result.error()));
    }

    return HttpResponseData{
        .status_code = result->status,
        .body = result->body,
        .headers = {result->headers.begin(), result->headers.end()},
    };
}

// ---------------------------------------------------------------------------
// Static convenience: one-shot GET
// ---------------------------------------------------------------------------

std::optional<std::string> HttplibHttpClient::get_body(std::string_view url, std::optional<address_family> af,
                                                       const std::optional<std::string> &interface) {
    http_request req;
    req.url = url;
    req.request_method = http_method_type::GET;

    HttplibHttpClient client(af.value_or(address_family::UNSPECIFIED), interface);
    auto resp = client.send(req);

    if (!resp) {
        SPDLOG_WARN(R"(Failed to fetch "{}", error: {})", url, resp.error());
        return std::nullopt;
    }

    return resp->body;
}
