//
// Created by Kotarou on 2022/4/5.
//
#include "cloudflare.h"

#include "response.hpp"

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Services;
using yaddnsc::sdk::UpdateContext;
using yaddnsc::sdk::UpdateRequest;

namespace {
    constexpr std::string_view API_URL = "https://api.cloudflare.com/client/v4/zones/{ZONE_ID}/dns_records/{RECORD_ID}";
    constexpr std::string_view DRIVER_NAME = "cloudflare";
}

YADDNSC_DEFINE_DRIVER(CloudflareDriver, "cloudflare", "Updates DNS records via the Cloudflare API", "Kotarou",
                      "2.0.0", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result CloudflareDriver::update(UpdateContext &context) {
    const auto &params = context.request();
    const auto cfg = parse_config<CloudflareParams>(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("ZONE_ID", cfg.zone_id), fmt::arg("RECORD_ID", cfg.record_id));
    request.headers.push_back({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.body = generate_body(cfg, params);
    request.content_type = "application/json";
    request.method = Method::Put;

    YADDNSC_SDK_LOG_DEBUG(context, "Domain {} ({}) received DNS record update request from driver {}, {}",
                          params.fqdn, params.record_type, DRIVER_NAME, yaddnsc::sdk::format_request(request));

    auto response = context.exchange(request);
    if (!response) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update failed (HTTP error: {})", params.fqdn,
                             params.record_type, response.error().message);
        return std::unexpected(Error{response.error().status, response.error().message, 0});
    }

    if (!check_response(*response, context.services())) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update rejected by upstream", params.fqdn, params.record_type);
        return std::unexpected(Error{YADDNSC_STATUS_UPSTREAM_REJECTED,
                                     fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn,
                                                 params.record_type),
                                     0});
    }

    return {};
}

bool CloudflareDriver::check_response(const HttpResponse &response, const Services &services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    auto result = glz::read_json<CloudflareResponse>(response.body);
    if (!result) {
        YADDNSC_SDK_LOG_ERROR(services, "Failed to parse Cloudflare API response");
        return false;
    }

    auto &resp = result.value();
    if (!resp.success) {
        for (const auto &error: resp.errors) {
            if (error.source.has_value()) {
                YADDNSC_SDK_LOG_ERROR(services, "Cloudflare API error ({}): {} [{}]", error.code, error.message,
                                      error.source->pointer);
            } else {
                YADDNSC_SDK_LOG_ERROR(services, "Cloudflare API error ({}): {}", error.code, error.message);
            }
        }
        return false;
    }

    if (resp.result.has_value()) {
        auto &record = resp.result.value();
        YADDNSC_SDK_LOG_DEBUG(services,
                              "DNS record updated successfully: {} {} -> {} (TTL: {}, proxied: {})", record.type,
                              record.name, record.content, record.ttl, record.proxied ? "yes" : "no");
    }

    return true;
}

std::string CloudflareDriver::generate_body(const CloudflareParams &cfg, const UpdateRequest &request) {
    auto body = CloudflareRequestBody{
        .type = std::string(request.record_type),
        .name = std::string(request.subdomain),
        .content = std::string(request.ip_address),
        .ttl = cfg.ttl.value_or(30),
        .proxied = cfg.proxied.value_or(false)
    };
    return glz::write_json(body).value_or("{}");
}
