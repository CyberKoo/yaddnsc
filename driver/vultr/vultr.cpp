//
// Created by Kotarou on 2026/7/13.
//

#include "vultr.h"

#include "fmt.hpp"
#include "config.hpp"
#include "response.hpp"
#include "driver/factory.h"
#include "interface/core_logger.h"

namespace {
    constexpr std::string_view API_URL = "https://api.vultr.com/v2/domains/{DOMAIN}/records/{RECORD_ID}";
}

DEFINE_DRIVER_FACTORY(VultrDriver)

DriverRequestContext VultrDriver::generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const {
    auto cfg = parse_config<VultrParams>(config);

    auto url = fmt::format(API_URL,
                           fmt::arg("DOMAIN", ctx.domain),
                           fmt::arg("RECORD_ID", cfg.record_id));

    auto body = VultrRequestBody{
        .name = ctx.subdomain,
        .data = ctx.ip_addr,
        .ttl = cfg.ttl
    };

    DriverRequest request{};
    request.headers.insert({"Authorization", fmt::format("Bearer {}", cfg.api_key)});
    request.body = glz::write_json(body).value_or("{}");
    request.content_type = "application/json";
    request.method = DriverHttpMethod::PATCH;

    return {std::move(url), std::move(request)};
}

bool VultrDriver::check_response(const HttpResponse &response) const {
    CORE_LOG_TRACE("Got {} from server.", response.body);

    // Vultr returns 204 No Content with an empty body on success.
    if (response.status_code == 204) {
        CORE_LOG_DEBUG("DNS record updated successfully");
        return true;
    }

    // Error responses include a JSON body with error details.
    if (!response.body.empty()) {
        if (auto result = glz::read_json<VultrErrorResponse>(response.body)) {
            for (const auto &err : result.value().errors) {
                CORE_LOG_ERROR("Vultr API error: {}", err.detail);
            }
        } else {
            CORE_LOG_ERROR("Vultr API error (HTTP {}): {}", response.status_code, response.body);
        }
    } else {
        CORE_LOG_ERROR("Vultr API request failed with HTTP status {}", response.status_code);
    }

    return false;
}

DriverDetail VultrDriver::get_detail() const noexcept {
    return {
        .name = "vultr",
        .description = "Updates DNS records via the Vultr API",
        .author = "Kotarou",
        .version = "1.0.0"
    };
}
