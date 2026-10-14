#include "duckdns.h"

#include <optional>
#include <vector>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::vector member complete; clangd sees no spelled use

#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/redact.hpp>

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
using yaddnsc::sdk::redact::truncate_for_log;

namespace {
constexpr std::string_view API_URL = "https://www.duckdns.org/update";
constexpr std::string_view DRIVER_NAME = "duckdns";
}  // namespace

YADDNSC_DEFINE_DRIVER(DuckDnsDriver, "duckdns", "Updates DNS records via the DuckDNS API", "Kotarou", "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA, 0)

DuckDnsParams DuckDnsDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<DuckDnsParams>(json);
    if (cfg.token.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'token' must not be empty");
    }
    return cfg;
}

Result DuckDnsDriver::validate(std::string_view driver_param_json) const {
    [[maybe_unused]] const auto cfg = parse_params(driver_param_json);
    return {};
}

Result DuckDnsDriver::update(UpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    HttpRequest request{};
    request.url = generate_url(cfg, params);
    request.method = Method::GET;

    return run_update(context, DRIVER_NAME, request, check_response);
}

bool DuckDnsDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server", truncate_for_log(response.body));

    // DuckDNS returns:
    //   "OK"           — success (non-verbose)
    //   "OK\n..."      — success (verbose mode)
    //   "KO"           — failure
    // Acceptance is reported in the body, so the body decides the outcome and
    // a success body wins over the status code.
    if (response.body.starts_with("OK")) {
        YADDNSC_SDK_LOG_INFO(services, "DNS record updated successfully");
        return true;
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (yaddnsc::sdk::detail::is_transient_status(response.status_code)) {
        YADDNSC_SDK_LOG_WARN(services, "DuckDNS request failed with HTTP status {}: {}", response.status_code,
                             truncate_for_log(response.body));
        return false;
    }

    YADDNSC_SDK_LOG_ERROR(services, "DuckDNS API error: {}", truncate_for_log(response.body));
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
