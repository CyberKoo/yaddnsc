//
// Created by Kotarou on 2026/7/13.
//

#include "duckdns.h"

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
}

YADDNSC_DEFINE_DRIVER(DuckDnsDriver, "duckdns", "Updates DNS records via the DuckDNS API", "Kotarou", "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result DuckDnsDriver::update(UpdateContext &context) {
    const auto &params = context.request();
    const auto cfg = parse_config<DuckDnsParams>(params.driver_param_json);

    HttpRequest request{};
    request.url = generate_url(cfg, params);
    request.method = Method::Get;

    YADDNSC_SDK_LOG_DEBUG(context, "Domain {} ({}) received DNS record update request from driver {}, {}",
                          params.fqdn, params.record_type, DRIVER_NAME, yaddnsc::sdk::format_request(request));

    auto response = context.exchange(request);
    if (!response) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update failed (HTTP error: {})", params.fqdn,
                             params.record_type, response.error().message);
        return std::unexpected(Error{response.error().status, response.error().message, 0});
    }

    if (!check_response(*response, context.services())) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update rejected by upstream", params.fqdn, params.record_type);
        return std::unexpected(Error{YADDNSC_STATUS_UPSTREAM_REJECTED,
                                     fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn,
                                                 params.record_type),
                                     0});
    }

    return {};
}

bool DuckDnsDriver::check_response(const HttpResponse &response, const Services &services) {
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

std::string DuckDnsDriver::generate_url(const DuckDnsParams &cfg, const UpdateRequest &params) {
    // Use ipv6 param for AAAA records, ip param for A records
    auto ip_param = (params.record_type == "AAAA") ? "ipv6" : "ip";

    auto url = fmt::format("{}?domains={}&token={}&{}={}",
                           API_URL, params.subdomain, cfg.token, ip_param, params.ip_address);

    if (cfg.verbose.value_or(false)) {
        url += "&verbose=true";
    }

    return url;
}
