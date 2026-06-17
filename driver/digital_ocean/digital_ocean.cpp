//
// Created by Kotarou on 2022/4/5.
//

#include "digital_ocean.h"
#include "response.h"

#include <glaze/glaze.hpp>

DEFINE_DRIVER_CREATE(DigitalOceanDriver)

constexpr char API_URL[] = "https://api.digitalocean.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";

driver_request DigitalOceanDriver::generate_request(const driver_config_type &config) const {
    check_required_params(config);

    driver_request request{};
    request.header.insert({"Authorization", fmt::format("Bearer {}", config.at("token"))});
    request.url = fmt::format(API_URL,
                                fmt::arg("DOMAIN", config.at("domain")),
                                fmt::arg("RECORD_ID", config.at("record_id")));
    auto do_body = glz::obj{"data", config.at("ip_addr")};
    request.body = glz::write_json(do_body).value_or("{}");
    request.content_type = "application/json";
    request.request_method = driver_http_method_type::PUT;

    return request;
}

bool DigitalOceanDriver::check_response(std::string_view response) const {
    CORE_LOG_TRACE("Got {} from server.", response);

    // Try success response: { "domain_record": { ... } }
    if (auto result = glz::read_json<DigitalOceanDomainResponse>(response)) {
        auto &record = result.value().domain_record;
        CORE_LOG_INFO("DNS record updated successfully: {} {} -> {} (TTL: {})",
                       record.type, record.name, record.data, record.ttl);
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

driver_detail DigitalOceanDriver::get_detail() const {
    return {
        .name = "digital_ocean",
        .description = "Digital Ocean DDNS driver",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}
