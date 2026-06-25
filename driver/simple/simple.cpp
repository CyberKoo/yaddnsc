//
// Created by Kotarou on 2022/4/5.
//

#include "simple.h"

#include <glaze/glaze.hpp>

#include "driver/driver_factory.h"
#include "fmt.hpp"
#include "interfaces/core_logger.h"

DEFINE_DRIVER_FACTORY(SimpleDriver)

driver_request SimpleDriver::generate_request(const driver_config_type &config, const UpdateContext &ctx) const {
    auto full = parse_config<glz::generic>(config);
    if (!full.is_object() || !full.contains("url") || !full["url"].is_string()) {
        throw ParamParseException(fmt::format("Missing required parameter \"url\" in driver config"));
    }

    auto &obj = full.get_object();
    auto url = obj["url"].get_string();

    // Substitute all keys into the URL template: config params first, then context
    auto substitute = [&](std::string_view key, std::string_view val) {
        const auto target = fmt::format("{{{}}}", key);
        for (auto pos = url.find(target); pos != std::string::npos;
             pos = url.find(target, pos + val.size())) {
            url.replace(pos, target.size(), val);
        }
    };

    for (auto &[key, val]: obj) {
        if (key != "url" && val.is_string()) {
            substitute(key, val.get_string());
        }
    }

    substitute("ip_addr", ctx.ip_addr);
    substitute("rd_type", ctx.rd_type);
    substitute("domain", ctx.domain);
    substitute("subdomain", ctx.subdomain);
    substitute("fqdn", ctx.fqdn);

    return {
        .url = std::move(url), .body = std::string{}, .content_type = std::string{},
        .request_method = driver_http_method_type::GET, .header = {}
    };
}

bool SimpleDriver::check_response(std::string_view response) const {
    CORE_LOG_DEBUG("Response: {}", response);
    return !response.empty();
}

DriverDetail SimpleDriver::get_detail() const {
    return {
        .name = "simple",
        .description = "Simple HTTP driver",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}
