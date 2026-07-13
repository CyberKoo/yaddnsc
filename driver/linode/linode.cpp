//
// Created by Kotarou on 2026/7/13.
//

#include "linode.h"

#include "fmt.hpp"
#include "config.hpp"
#include "response.hpp"
#include "driver/factory.h"
#include "interface/core_logger.h"

namespace {
    constexpr std::string_view API_URL = "https://api.linode.com/v4/domains/{DOMAIN_ID}/records/{RECORD_ID}";
}

DEFINE_DRIVER_FACTORY(LinodeDriver)

DriverRequestContext LinodeDriver::generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const {
    auto cfg = parse_config<LinodeParams>(config);

    auto url = fmt::format(API_URL,
                           fmt::arg("DOMAIN_ID", cfg.domain_id),
                           fmt::arg("RECORD_ID", cfg.record_id));

    auto body = LinodeRequestBody{
        .name = ctx.subdomain,
        .target = ctx.ip_addr,
        .ttl_sec = cfg.ttl_sec
    };

    DriverRequest request{};
    request.headers.insert({"Authorization", fmt::format("Bearer {}", cfg.token)});
    request.body = glz::write_json(body).value_or("{}");
    request.content_type = "application/json";
    request.method = DriverHttpMethod::PUT;

    return {std::move(url), std::move(request)};
}

bool LinodeDriver::check_response(const HttpResponse &response) const {
    CORE_LOG_TRACE("Got {} from server.", response.body);

    // Linode returns 200 OK with the updated record object on success.
    if (response.status_code == 200) {
        CORE_LOG_DEBUG("DNS record updated successfully");
        return true;
    }

    // Error responses include a JSON body with error details.
    if (!response.body.empty()) {
        if (auto result = glz::read_json<LinodeErrorResponse>(response.body)) {
            for (const auto &err : result.value().errors) {
                CORE_LOG_ERROR("Linode API error{}: {}", err.field.empty() ? "" : fmt::format(" ({})", err.field),
                               err.reason);
            }
        } else {
            CORE_LOG_ERROR("Linode API error (HTTP {}): {}", response.status_code, response.body);
        }
    } else {
        CORE_LOG_ERROR("Linode API request failed with HTTP status {}", response.status_code);
    }

    return false;
}

DriverDetail LinodeDriver::get_detail() const noexcept {
    return {
        .name = "linode",
        .description = "Updates DNS records via the Linode API",
        .author = "Kotarou",
        .version = "1.0.0"
    };
}
