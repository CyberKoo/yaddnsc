//
// Created by Kotarou on 2026/7/13.
//

#include "duckdns.h"

#include <optional>
#include <vector>

#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/util/format.hpp>

#include "config.hpp"

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Services;
using yaddnsc::sdk::UpdateContext;
using yaddnsc::sdk::UpdateRequest;

namespace {
constexpr std::string_view API_URL = "https://www.duckdns.org/update";
constexpr std::string_view DRIVER_NAME = "duckdns";
}  // namespace

YADDNSC_DEFINE_DRIVER(DuckDnsDriver,
                      "duckdns",
                      "Updates DNS records via the DuckDNS API",
                      "Kotarou",
                      "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result DuckDnsDriver::validate(std::string_view driver_param_json) const {
    // Reuses the update-time schema: parse_config throws ConfigParseError on
    // missing keys or malformed values, which the ABI entry maps to
    // YADDNSC_STATUS_INVALID_CONFIG.
    [[maybe_unused]] const auto cfg = parse_config<DuckDnsParams>(driver_param_json);
    return {};
}

Result DuckDnsDriver::update(UpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_config<DuckDnsParams>(params.driver_param_json);

    HttpRequest request{};
    request.url = generate_url(cfg, params);
    request.method = Method::Get;

    return run_update(context, DRIVER_NAME, request, [](const HttpResponse& response, const Services& services) {
        return check_response(response, services);
    });
}

bool DuckDnsDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    // DuckDNS returns:
    //   "OK"           — success (non-verbose)
    //   "OK\n..."      — success (verbose mode)
    //   "KO"           — failure
    if (response.body.starts_with("OK")) {
        if (response.body.size() > 2) {
            YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully: {}", response.body);
        }
        return true;
    }

    YADDNSC_SDK_LOG_ERROR(services, "DuckDNS API error: {}", response.body);
    return false;
}

std::string DuckDnsDriver::generate_url(const DuckDnsParams& cfg, const UpdateRequest& params) {
    // Use ipv6 param for AAAA records, ip param for A records
    auto ip_param = (params.record_type == "AAAA") ? "ipv6" : "ip";

    auto url =
        fmt::format("{}?domains={}&token={}&{}={}", API_URL, params.subdomain, cfg.token, ip_param, params.ip_address);

    if (cfg.verbose.value_or(false)) {
        url += "&verbose=true";
    }

    return url;
}
