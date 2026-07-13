//
// Created by Kotarou on 2026/7/13.
//

#include "porkbun.h"

#include "fmt.hpp"
#include "config.hpp"
#include "response.hpp"
#include "driver/factory.h"
#include "interface/core_logger.h"

namespace {
    constexpr std::string_view API_URL = "https://api.porkbun.com/api/json/v3/dns/editByNameType/{DOMAIN}/{TYPE}/{SUBDOMAIN}";
}

DEFINE_DRIVER_FACTORY(PorkbunDriver)

DriverRequestContext PorkbunDriver::generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const {
    auto cfg = parse_config<PorkbunParams>(config);

    // Porkbun's editByNameType uses subdomain (not FQDN). Empty subdomain for root domain.
    auto subdomain = (ctx.subdomain == "@" || ctx.subdomain.empty()) ? "" : ctx.subdomain;

    auto url = fmt::format(API_URL,
                           fmt::arg("DOMAIN", ctx.domain),
                           fmt::arg("TYPE", ctx.rd_type),
                           fmt::arg("SUBDOMAIN", subdomain));

    auto body = PorkbunRequestBody{
        .apikey = cfg.api_key,
        .secretapikey = cfg.secret_api_key,
        .content = ctx.ip_addr,
        .ttl = cfg.ttl
    };

    DriverRequest request{};
    // Use header auth (preferred per docs) and body auth as fallback
    request.headers.insert({"X-API-Key", cfg.api_key});
    request.headers.insert({"X-Secret-API-Key", cfg.secret_api_key});
    request.body = glz::write_json(body).value_or("{}");
    request.content_type = "application/json";
    request.method = DriverHttpMethod::POST;

    return {std::move(url), std::move(request)};
}

bool PorkbunDriver::check_response(const HttpResponse &response) const {
    CORE_LOG_TRACE("Got {} from server.", response.body);

    auto result = glz::read_json<PorkbunResponse>(response.body);
    if (!result) {
        CORE_LOG_ERROR("Failed to parse Porkbun API response");
        return false;
    }

    auto &resp = result.value();
    if (resp.status == "SUCCESS") {
        CORE_LOG_DEBUG("DNS record updated successfully");
        return true;
    }

    if (resp.message.has_value()) {
        CORE_LOG_ERROR("Porkbun API error ({}): {}", resp.code.value_or("unknown"), resp.message.value());
    } else {
        CORE_LOG_ERROR("Porkbun API request failed with status: {}", resp.status);
    }

    return false;
}

DriverDetail PorkbunDriver::get_detail() const noexcept {
    return {
        .name = "porkbun",
        .description = "Updates DNS records via the Porkbun API",
        .author = "Kotarou",
        .version = "1.0.0"
    };
}
