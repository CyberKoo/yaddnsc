//
// Created by Kotarou on 2022/4/5.
//

#include "digital_ocean.h"

#include "fmt.hpp"
#include "config.hpp"
#include "response.h"
#include "driver/driver_factory.h"
#include "interfaces/core_logger.h"

namespace {
    constexpr std::string_view API_URL = "https://api.digitalocean.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";
}

DEFINE_DRIVER_FACTORY(DigitalOceanDriver)

driver_request DigitalOceanDriver::generate_request(const driver_config_type &config, const UpdateContext &ctx) const {
    auto cfg = parse_config<DigitalOceanParams>(config);

    driver_request request{};
    request.header.insert({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN", ctx.domain), fmt::arg("RECORD_ID", cfg.record_id));
    auto request_body = DigitalOceanBody{.data = ctx.ip_addr};
    request.body = glz::write_json(request_body).value_or("{}");
    request.content_type = "application/json";
    request.request_method = driver_http_method_type::PUT;

    return request;
}

bool DigitalOceanDriver::check_response(std::string_view response) const {
    CORE_LOG_TRACE("Got {} from server.", response);

    // Try success response: { "domain_record": { ... } }
    if (auto result = glz::read_json<DigitalOceanDomainResponse>(response)) {
        auto &record = result.value().domain_record;
        CORE_LOG_DEBUG("DNS record updated successfully: {} {} -> {} (TTL: {})", record.type, record.name, record.data,
                       record.ttl);
        return true;
    }

    // Try error response: { "id": "...", "message": "..." }
    if (auto result = glz::read_json<DigitalOceanErrorResponse>(response)) {
        auto &err = result.value();
        CORE_LOG_ERROR("DigitalOcean API error ({}): {}", err.id, err.message);
        return false;
    }

    CORE_LOG_ERROR("Failed to parse DigitalOcean API response");
    return false;
}

DriverDetail DigitalOceanDriver::get_detail() const {
    return {
        .name = "digital_ocean",
        .description = "Updates DNS records via the DigitalOcean API",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}
