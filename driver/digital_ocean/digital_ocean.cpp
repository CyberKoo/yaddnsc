//
// Created by Kotarou on 2022/4/5.
//

#include "digital_ocean.h"
#include "core_logger.h"
#include "response.h"

#include <glaze/glaze.hpp>

DEFINE_DRIVER_CREATE(DigitalOceanDriver)

constexpr char API_URL[] = "https://api.digitalocean.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";

DigitalOceanDriver::DigitalOceanDriver() {
    required_param_.emplace_back("domain");
    required_param_.emplace_back("record_id");
    required_param_.emplace_back("token");
    required_param_.emplace_back("ip_addr");
}

driver_request DigitalOceanDriver::generate_request(const driver_config_type &config) const {
    check_required_params(config);

    driver_request request{};
    request.header.insert({"Authorization", fmt::format("Bearer {}", config.at("token"))});
    request.url = fmt::vformat(
        API_URL, std::map<std::string, std::string>{
            {"DOMAIN", config.at("domain")},
            {"RECORD_ID", config.at("record_id")}
        }
    );
    auto do_body = glz::obj{"data", config.at("ip_addr")};
    request.body = glz::write_json(do_body).value_or("{}");
    request.content_type = "application/json";
    request.request_method = driver_http_method_type::PUT;

    return request;
}

bool DigitalOceanDriver::check_response(std::string_view response) const {
    CORE_LOG_TRACE("Got {} from server.", response);

    DigitalOceanResponse resp{};
    if (auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(resp, response)) {
        CORE_LOG_ERROR("Failed to parse DigitalOcean API response");
        return false;
    }

    if (resp.message.has_value()) {
        CORE_LOG_ERROR("Error from server: {}", resp.message.value());
        return false;
    }

    return true;
}

driver_detail DigitalOceanDriver::get_detail() const {
    return {
        .name = "digital_ocean",
        .description = "Digital Ocean DDNS driver",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}
