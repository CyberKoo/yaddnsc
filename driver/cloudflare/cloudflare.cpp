//
// Created by Kotarou on 2022/4/5.
//
#include "cloudflare.h"

#include "core_logger.h"
#include "fmt.hpp"
#include <driver/driver_factory.h>

#include "config.hpp"
#include "response.h"

DEFINE_DRIVER_FACTORY(CloudflareDriver)

constexpr std::string_view API_URL = "https://api.cloudflare.com/client/v4/zones/{ZONE_ID}/dns_records/{RECORD_ID}";

driver_request CloudflareDriver::generate_request(
    const driver_config_type &config, const UpdateContext &ctx) const {
    auto cfg = parse_config<CloudflareParams>(config);

    driver_request request{};
    request.header.insert({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.url = fmt::format(API_URL, fmt::arg("ZONE_ID", cfg.zone_id), fmt::arg("RECORD_ID", cfg.record_id));
    request.body = generate_body(cfg, ctx);
    request.content_type = "application/json";
    request.request_method = driver_http_method_type::PUT;

    return request;
}

bool CloudflareDriver::check_response(std::string_view response) const {
    CORE_LOG_TRACE("Got {} from server.", response);

    auto result = glz::read_json<CloudflareResponse>(response);
    if (!result) {
        CORE_LOG_ERROR("Failed to parse Cloudflare API response");
        return false;
    }

    auto &resp = result.value();
    if (!resp.success) {
        for (const auto &error: resp.errors) {
            if (error.source.has_value()) {
                CORE_LOG_ERROR("Cloudflare API error ({}): {} [{}]", error.code, error.message, error.source->pointer);
            } else {
                CORE_LOG_ERROR("Cloudflare API error ({}): {}", error.code, error.message);
            }
        }
        return false;
    }

    if (resp.result.has_value()) {
        auto &record = resp.result.value();
        CORE_LOG_DEBUG("DNS record updated successfully: {} {} -> {} (TTL: {}, proxied: {})", record.type, record.name,
                       record.content, record.ttl, record.proxied ? "yes" : "no");
    }

    return true;
}

std::string CloudflareDriver::generate_body(const CloudflareParams &cfg, const UpdateContext &ctx) {
    auto body = CloudflareRequestBody{
        .type = ctx.rd_type,
        .name = ctx.subdomain,
        .content = ctx.ip_addr,
        .ttl = cfg.ttl.value_or(30),
        .proxied = cfg.proxied.value_or(false)
    };
    return glz::write_json(body).value_or("{}");
}

DriverDetail CloudflareDriver::get_detail() const {
    return {
        .name = "cloudflare",
        .description = "Cloudflare DDNS driver",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}
