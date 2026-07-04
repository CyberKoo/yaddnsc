//
// Created by Kotarou on 2022/4/5.
//
#include "http_client.h"

#include <sys/socket.h>

#include <ranges>
#include <utility>
#include <optional>
#include <filesystem>

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include "uri.h"
#include "fmt.hpp"
#include "version.h"
#include "http_type.h"
#include "util/cert_util.hpp"

namespace {
    std::string build_request(const Uri &uri) {
        if (uri.get_query_string().empty()) {
            return std::string(uri.get_path());
        }

        return fmt::format("{}?{}", uri.get_path(), uri.get_query_string());
    }

    std::string build_base_url(const Uri &uri) {
        return fmt::format("{}://{}:{}", uri.get_schema(), uri.get_host(), uri.get_port());
    }

    // -----------------------------------------------------------------------
    // Dispatch: HttpMethod -> httplib callable
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

    // Apply all configured (or defaulted) options from HttpClientOptions onto
    // a freshly created httplib::Client.
    void apply_options(httplib::Client &client, const Uri &uri, const HttpClientOptions &opts) {
        // --- TLS / CA --------------------------------------------------------
        if (uri.get_schema() == "https") {
            std::optional<std::string> ca_path;

            if (opts.ca_cert_path.has_value()) {
                ca_path = opts.ca_cert_path;
            } else {
                // fall back to auto-detection
                ca_path = Utils::Cert::get_system_ca_path();
            }

            if (ca_path.has_value()) {
                client.set_ca_cert_path(*ca_path);
                client.enable_server_certificate_verification(opts.verify_server_cert.value_or(true));
            }
        }

        // --- Outbound interface ----------------------------------------------
        if (opts.interface.has_value() && !opts.interface->empty()) {
            client.set_interface(*opts.interface);
        }

        // --- Address family --------------------------------------------------
        switch (opts.address_family.value_or(AddressFamily::UNSPECIFIED)) {
            case AddressFamily::IPV4:
                client.set_address_family(AF_INET);
                break;
            case AddressFamily::IPV6:
                client.set_address_family(AF_INET6);
                break;
            default:
                client.set_address_family(AF_UNSPEC);
                break;
        }

        // --- Timeouts --------------------------------------------------------
        client.set_connection_timeout(opts.connection_timeout.value_or(std::chrono::seconds(5)));
        client.set_read_timeout(opts.read_timeout.value_or(std::chrono::seconds(5)));
        if (opts.write_timeout.has_value()) {
            client.set_write_timeout(*opts.write_timeout);
        }

        // --- Redirects -------------------------------------------------------
        client.set_follow_location(opts.follow_location.value_or(true));

        // --- Headers ---------------------------------------------------------
        httplib::Headers default_headers;
        default_headers.emplace("User-Agent", yaddnsc::get_full_version());

        if (opts.default_headers.has_value()) {
            for (const auto &[k, v]: *opts.default_headers) {
                default_headers.emplace(k, v);
            }
        }

        client.set_default_headers(std::move(default_headers));
    }
}

// ---------------------------------------------------------------------------
// HttpClient (static)
// ---------------------------------------------------------------------------

std::string HttpClient::params_to_query_string(const HttpParams &params) {
    auto encoded = params | std::views::transform([](const auto &p) {
        return fmt::format("{}={}",
                           httplib::encode_query_component(p.first),
                           httplib::encode_query_component(p.second)
        );
    });
    return fmt::format("{}", fmt::join(encoded, "&"));
}

// ---------------------------------------------------------------------------
// TransientHttpClient
// ---------------------------------------------------------------------------

TransientHttpClient::TransientHttpClient(HttpClientOptions opts) : opts_(std::move(opts)) {
}

HttpResult TransientHttpClient::send(const HttpRequest &req) const {
    const auto uri = Uri::parse(req.url);

    SPDLOG_DEBUG("Sending {} request to {}://{}{} ({} header(s), {} bytes body)",
                 magic_enum::enum_name(req.method), uri.get_schema(), uri.get_host(), build_request(uri),
                 req.headers.size(), req.body ? req.body->size() : 0
    );

    auto client = httplib::Client(build_base_url(uri));
    apply_options(client, uri, opts_);

    httplib::Headers headers{req.headers.begin(), req.headers.end()};
    const auto path = build_request(uri);

    const auto result = INVOKERS[std::to_underlying(req.method)](
        client, path.c_str(), headers, req.body, req.content_type.data()
    );

    if (!result) {
        auto error_str = httplib::to_string(result.error());
        SPDLOG_DEBUG("HTTP request to {}://{}{} failed: {}", uri.get_schema(), uri.get_host(), path, error_str);
        return std::unexpected(error_str);
    }

    SPDLOG_DEBUG("Received {} response from {}://{}{} (status {}, {} bytes)", magic_enum::enum_name(req.method),
                 uri.get_schema(), uri.get_host(), path, result->status, result->body.size()
    );

    return HttpResponse{
        .status_code = result->status, .body = result->body,
        .headers = {result->headers.begin(), result->headers.end()},
    };
}

// One-shot GET — convenience.
std::optional<std::string> TransientHttpClient::get_body(std::string_view url, const HttpClientOptions &opts) {
    HttpRequest req;
    req.url = url;
    req.method = HttpMethod::GET;

    TransientHttpClient client(opts);
    auto resp = client.send(req);

    if (!resp) {
        SPDLOG_DEBUG(R"(Failed to fetch "{}", error: {})", url, resp.error());
        return std::nullopt;
    }

    return resp->body;
}

// ---------------------------------------------------------------------------
// PersistentHttpClient
// ---------------------------------------------------------------------------

PersistentHttpClient::PersistentHttpClient(const Uri &uri, HttpClientOptions opts) : client_(
    std::make_unique<httplib::Client>(build_base_url(uri))) {
    apply_options(*client_, uri, opts);
}

PersistentHttpClient::~PersistentHttpClient() = default;

HttpResult PersistentHttpClient::send(const HttpRequest &req) const {
    const auto uri = Uri::parse(req.url);
    const auto path = build_request(uri);

    SPDLOG_DEBUG("Sending {} request to {}://{}{} ({} header(s), {} bytes body)",
                 magic_enum::enum_name(req.method), uri.get_schema(), uri.get_host(), path,
                 req.headers.size(), req.body ? req.body->size() : 0);

    httplib::Headers headers{req.headers.begin(), req.headers.end()};

    const auto result = INVOKERS[std::to_underlying(req.method)](
        *client_, path.c_str(), headers, req.body, req.content_type.data()
    );

    if (!result) {
        auto error_str = httplib::to_string(result.error());
        SPDLOG_DEBUG("HTTP request to {}://{}{} failed: {}", uri.get_schema(), uri.get_host(), path, error_str);
        return std::unexpected(error_str);
    }

    SPDLOG_DEBUG("Received {} response from {}://{}{} (status {}, {} bytes)",
                 magic_enum::enum_name(req.method),
                 uri.get_schema(), uri.get_host(), path, result->status, result->body.size()
    );

    return HttpResponse{
        .status_code = result->status, .body = result->body,
        .headers = {result->headers.begin(), result->headers.end()},
    };
}
