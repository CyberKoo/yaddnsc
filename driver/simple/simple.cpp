//
// Created by Kotarou on 2022/4/5.
//

#include "simple.h"

#include <map>

DEFINE_DRIVER_CREATE(SimpleDriver)

driver_request SimpleDriver::generate_request(const driver_config_type &config) const {
    check_required_params(config);

    auto url = config.at("url");

    // Substitute every known parameter (except "url" itself) into the URL
    // template.  For each key present in config, any "{key}" placeholder in
    // the url is replaced with the corresponding value.
    for (const auto &[key, val]: config) {
        if (key == "url") continue;

        const auto placeholder = fmt::format("{{{}}}", key);
        for (auto pos = url.find(placeholder); pos != std::string::npos;
             pos = url.find(placeholder, pos + val.size())) {
            url.replace(pos, placeholder.size(), val);
        }
    }

    return {
        .url = std::move(url), .body = std::string{}, .content_type = std::string{},
        .request_method = driver_http_method_type::GET, .header = {}
    };
}

bool SimpleDriver::check_response(std::string_view response) const {
    return !response.empty();
}

driver_detail SimpleDriver::get_detail() const {
    return {
        .name = "simple",
        .description = "Simple HTTP driver",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}
