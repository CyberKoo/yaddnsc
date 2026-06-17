//
// Created by Kotarou on 2022/4/5.
//


#include "cloudflare.h"
#include "string_util.h"
#include "response.h"

#include <charconv>

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
    request.url = fmt::vformat(
        API_URL, std::map<std::string, std::string>{
            {"ZONE_ID", config.at("zone_id")},
            {"RECORD_ID", config.at("record_id")}
        }
    );
    request.body = generate_body(config);
    request.content_type = "application/json";
    request.request_method = driver_http_method_type::PUT;

    return request;
}

bool CloudflareDriver::check_response(std::string_view response) const {
    auto result = glz::read_json<CloudflareResponse>(response);
    if (!result) {
        CORE_LOG_ERROR("Failed to parse Cloudflare API response");
        return false;
    }

    auto &resp = result.value();
    if (!resp.success) {
        for (const auto &error: resp.errors) {
            CORE_LOG_ERROR("Cloudflare API error: {} ({})", error.message, error.code);
        }
    }
    return resp.success;
}

std::string CloudflareDriver::generate_body(const driver_config_type &config) {
    auto ttl_value = get_optional(config, "ttl").value_or("30");
    int ttl = 30;
    std::from_chars(ttl_value.data(), ttl_value.data() + ttl_value.size(), ttl);

    auto body = glz::obj{
        "type", config.at("rd_type"),
        "name", config.at("sub_domain"),
        "content", config.at("ip_addr"),
        "ttl", ttl,
        "proxied", StringUtil::str_to_bool(get_optional(config, "proxied").value_or("0"))
    };
    return glz::write_json(body).value_or("{}");
}

driver_detail CloudflareDriver::get_detail() const {
    return {
        .name = "cloudflare",
        .description = "Cloudflare DDNS driver",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}
