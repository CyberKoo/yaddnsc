//
// Created by Kotarou on 2022/4/5.
//
#include <map>

#include "simple.h"

SimpleDriver::SimpleDriver() {
    _required_param.emplace_back("url");
}

driver_request_t SimpleDriver::generate_request(const driver_config_t &config) const {
    check_required_params(config);

    auto url = config.at("url");
    auto format = get_optional(config, "format");
    if (format.has_value()) {
        url = vformat(url, get_format_params(config));
    }

    return {.url = url, .body="", .content_type = "", .request_method = driver_http_method_t::GET, .header = {}};
}

std::map<std::string, std::string> SimpleDriver::get_format_params(const driver_config_t &config) {
    std::map<std::string, std::string> params;
    for (auto &[key, val]: config) {
        if (key.front() == '{' and key.back() == '}') {
            params.emplace(key.substr(1, key.size() - 2), val);
        }
    }

    return params;
}

bool SimpleDriver::check_response([[maybe_unused]] std::string_view response) const {
    return true;
}

constexpr driver_detail_t SimpleDriver::get_detail() const {
    return {
            .name = "simple",
            .description="Simple HTTP driver",
            .author="Kotarou",
            .version = "1.0.0"
    };
}
