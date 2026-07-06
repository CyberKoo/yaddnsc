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

    [[nodiscard]] httplib::Result dispatch(httplib::Client &client, const char *path,
                                           const HttpRequest &req) {
        httplib::Headers headers{req.headers.begin(), req.headers.end()};

        switch (req.method) {
            using enum HttpMethod;
        case GET:
            return client.Get(path, headers);
        case POST:
            return client.Post(path, headers, req.body.value_or(""), req.content_type);
        case PUT:
            return client.Put(path, headers, req.body.value_or(""), req.content_type);
        case DEL:
            return client.Delete(path, headers);
        case PATCH:
            return client.Patch(path, headers, req.body.value_or(""), req.content_type);
        case HEAD:
            return client.Head(path, headers);
        case OPTIONS:
            return client.Options(path, headers);
        }

        std::unreachable();
    }

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

    // -----------------------------------------------------------------------
    // do_exchange — shared by TransientHttpClient and PersistentHttpClient
    // -----------------------------------------------------------------------

    [[nodiscard]] HttpResult do_exchange(httplib::Client &client, std::string_view url,
                                          const HttpRequest &req) {
        const auto uri = Uri::parse(url);
        const auto path = build_request(uri);

        SPDLOG_DEBUG("Sending {} request to {}://{}{} ({} header(s), {} bytes body)",
                     magic_enum::enum_name(req.method), uri.get_schema(), uri.get_host(), path,
                     req.headers.size(), req.body ? req.body->size() : 0);

        const auto result = dispatch(client, path.c_str(), req);

        if (!result) {
            auto error_str = httplib::to_string(result.error());
            SPDLOG_DEBUG("HTTP request to {}://{}{} failed: {}", uri.get_schema(), uri.get_host(), path, error_str);
            return std::unexpected(error_str);
        }

        SPDLOG_DEBUG("Received {} response from {}://{}{} (status {}, {} bytes)",
                     magic_enum::enum_name(req.method),uri.get_schema(), uri.get_host(), path, result->status, result->body.size());

        return HttpResponse{
            .status_code = result->status, .body = result->body,
            .headers = {result->headers.begin(), result->headers.end()},
        };
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

// One-shot GET — convenience, uses the instance's configured options.
std::optional<std::string> HttpClient::get_body(std::string_view url) const {
    HttpRequest req;
    req.method = HttpMethod::GET;

    auto resp = exchange(url, req);

    if (!resp) {
        SPDLOG_DEBUG(R"(Failed to fetch "{}", error: {})", url, resp.error());
        return std::nullopt;
    }

    return resp->body;
}

// ---------------------------------------------------------------------------
// TransientHttpClient
// ---------------------------------------------------------------------------

TransientHttpClient::TransientHttpClient(HttpClientOptions opts) : opts_(std::move(opts)) {
}

HttpResult TransientHttpClient::exchange(std::string_view url, const HttpRequest &req) const {
    const auto uri = Uri::parse(url);
    auto client = httplib::Client(build_base_url(uri));
    apply_options(client, uri, opts_);
    return do_exchange(client, url, req);
}

// ---------------------------------------------------------------------------
// PersistentHttpClient
// ---------------------------------------------------------------------------

PersistentHttpClient::PersistentHttpClient(const Uri &uri, HttpClientOptions opts) : client_(
    std::make_unique<httplib::Client>(build_base_url(uri))) {
    apply_options(*client_, uri, opts);
}

PersistentHttpClient::~PersistentHttpClient() = default;

HttpResult PersistentHttpClient::exchange(std::string_view url, const HttpRequest &req) const {
    return do_exchange(*client_, url, req);
}
