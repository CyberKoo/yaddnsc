//
// Created by Kotarou on 2022/4/5.
//
#include <nlohmann/json.hpp>

#include "cloudflare.h"
#include "string_util.h"

constexpr char API_URL[] = "https://api.cloudflare.com/client/v4/zones/{ZONE_ID}/dns_records/{RECORD_ID}";

CloudflareDriver::CloudflareDriver() {
    required_param_.emplace_back("sub_domain");
    required_param_.emplace_back("zone_id");
    required_param_.emplace_back("record_id");
    required_param_.emplace_back("token");
    required_param_.emplace_back("ip_addr");
    required_param_.emplace_back("rd_type");
}

driver_request CloudflareDriver::generate_request(const driver_config_type &config) const {
    check_required_params(config);

    driver_request request{};
    request.header.insert({"Authorization", fmt::format("Bearer {}", config.at("token"))});
    request.url = vformat(
        API_URL, {
            {"ZONE_ID", config.at("zone_id")},
            {"RECORD_ID", config.at("record_id")}
        }
    );
    request.body = generate_body(config);
    request.content_type = "application/json";
    request.request_method = driver_http_method_type::PUT;

    return request;
}

bool CloudflareDriver::check_response(std::string_view) const {
    return true;
}

std::string CloudflareDriver::generate_body(const driver_config_type &config) {
    nlohmann::json body;
    body["type"] = config.at("rd_type");
    body["name"] = config.at("sub_domain");
    body["content"] = config.at("ip_addr");
    body["ttl"] = std::stoi(get_optional(config, "ttl").value_or("30"));
    body["proxied"] = StringUtil::str_to_bool(get_optional(config, "proxied").value_or("0"));

    return body.dump();
}

driver_detail CloudflareDriver::get_detail() const {
    return {
        .name = "cloudflare",
        .description = "Cloudflare DDNS driver",
        .author = "Kotarou",
        .version = "1.0.0"
    };
}
