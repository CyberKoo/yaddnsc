//
// Created by Kotarou on 2026/7/13.
//

#include "godaddy.h"

#include <glaze/glaze.hpp>

#include "fmt.hpp"
#include "config.hpp"
#include "driver/factory.h"
#include "interface/core_logger.h"

namespace {
    constexpr std::string_view API_URL = "https://api.godaddy.com/v1/domains/{DOMAIN}/records/{TYPE}/{NAME}";

    /// GoDaddy DNS record update request body (single record in an array).
    struct GoDaddyRecordBody {
        std::string data;  ///< Record value (IP address)
        int ttl;           ///< Time-to-live in seconds
        std::string type;  ///< DNS record type (A, AAAA)
    };
} // anonymous namespace

template<>
struct glz::meta<GoDaddyRecordBody> {
    using T = GoDaddyRecordBody;
    static constexpr auto value = object(
        "data", &T::data,
        "ttl", &T::ttl,
        "type", &T::type
    );
};

DEFINE_DRIVER_FACTORY(GoDaddyDriver)

DriverRequestContext GoDaddyDriver::generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const {
    auto cfg = parse_config<GoDaddyParams>(config);

    auto url = fmt::format(API_URL,
                           fmt::arg("DOMAIN", ctx.domain),
                           fmt::arg("TYPE", ctx.rd_type),
                           fmt::arg("NAME", ctx.subdomain));

    auto body = GoDaddyRecordBody{
        .data = ctx.ip_addr,
        .ttl = cfg.ttl.value_or(600),
        .type = ctx.rd_type
    };

    // GoDaddy expects an array of records
    auto body_json = fmt::format("[{}]", glz::write_json(body).value_or("{}"));

    DriverRequest request{};
    request.headers.insert({"Authorization", fmt::format("sso-key {}:{}", cfg.key, cfg.secret)});
    request.body = std::move(body_json);
    request.content_type = "application/json";
    request.method = DriverHttpMethod::PUT;

    return {std::move(url), std::move(request)};
}

bool GoDaddyDriver::check_response(const HttpResponse &response) const {
    CORE_LOG_TRACE("Got {} from server.", response.body);

    // GoDaddy returns 200 OK with an empty body on success.
    if (response.status_code == 200) {
        CORE_LOG_DEBUG("DNS record updated successfully");
        return true;
    }

    // Error responses typically include a JSON body with error details.
    if (!response.body.empty()) {
        CORE_LOG_ERROR("GoDaddy API error (HTTP {}): {}", response.status_code, response.body);
    } else {
        CORE_LOG_ERROR("GoDaddy API request failed with HTTP status {}", response.status_code);
    }

    return false;
}

DriverDetail GoDaddyDriver::get_detail() const noexcept {
    return {
        .name = "godaddy",
        .description = "Updates DNS records via the GoDaddy API",
        .author = "Kotarou",
        .version = "1.0.0"
    };
}
