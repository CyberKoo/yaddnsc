//
// Created by Kotarou on 2022/4/5.
//

#include "simple.h"

#include <glaze/glaze.hpp>

#include "fmt.hpp"
#include "string_util.hpp"
#include "driver/factory.h"
#include "interface/core_logger.h"

DEFINE_DRIVER_FACTORY (SimpleDriver)

DriverRequestContext SimpleDriver::generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const {
    auto full = parse_config<glz::generic>(config);
    if (!full.is_object() || !full.contains("url") || !full["url"].is_string()) {
        throw ParamParseException(fmt::format("Missing required parameter \"url\" in driver config"));
    }

    auto &obj = full.get_object();
    auto url = obj["url"].get_string();

    // Substitute all keys into the URL template: config params first, then context
    const auto substitute = [&](std::string_view key, std::string_view val) {
        const auto target = fmt::format("{{{}}}", key);
        StringUtil::replace_all(url, target, val);
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
        .url = std::move(url),
        .request = {
            .content_type = std::string{},
            .method = DriverHttpMethod::GET,
            .headers = {},
            .body = std::nullopt,
        }
    };
}

DriverDetail SimpleDriver::get_detail() const noexcept {
    return {
        .name = "simple",
        .description = "Generic HTTP driver with URL template substitution",
        .author = "Kotarou",
        .version = "2.0.0"
    };
}

bool SimpleDriver::check_response(const HttpResponse &response) const {
    CORE_LOG_DEBUG("Status: {}, Response: {}", response.status_code, StringUtil::trim(response.body));

    if (response.status_code >= 300) {
        CORE_LOG_ERROR("HTTP request failed with status code {}", response.status_code);
        return false;
    }

    return !response.body.empty();
}
