#include "duckdns.h"

#include <expected>
#include <optional>
#include <string>
#include <vector>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::vector member complete; clangd sees no spelled use
#include <yaddnsc/sdk/log.hpp>
#include <yaddnsc/sdk/run_update.hpp>
#include <yaddnsc/sdk/upstream_failure.hpp>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/redact.hpp>

#include "config.hpp"

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::AsyncHttpResponse;
using yaddnsc::sdk::AsyncServices;
using yaddnsc::sdk::AsyncUpdateContext;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Task;
using yaddnsc::sdk::UpdateRequest;
using yaddnsc::sdk::redact::truncate_for_log;

namespace {
constexpr std::string_view API_URL = "https://www.duckdns.org/update";
constexpr std::string_view DRIVER_NAME = "duckdns";
/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "DuckDNS request";
}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(DuckDnsDriver, "duckdns", "Updates DNS records via the DuckDNS API", "Kotarou", "1.1.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

DuckDnsParams DuckDnsDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<DuckDnsParams>(json);
    require_non_empty("token", cfg.token);
    return cfg;
}

Result DuckDnsDriver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
}

Task<Result> DuckDnsDriver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    HttpRequest request{};
    request.url = generate_url(cfg, params);
    request.method = Method::GET;

    return run_update(context, DRIVER_NAME, request, check_response);
}

Result DuckDnsDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    // DuckDNS returns:
    //   "OK"           — success (non-verbose)
    //   "OK\n..."      — success (verbose mode)
    //   "KO"           — failure
    // Acceptance is reported in the body, so the body decides the outcome and
    // a success body wins over the status code.
    if (response.body.starts_with("OK")) {
        YADDNSC_ASYNC_SDK_LOG_INFO(services, "DNS record updated successfully");
        return {};
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    // The body carries the provider's own message.
    const auto text = fmt::format("DuckDNS API error: {}", truncate_for_log(response.body));
    YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
    return std::unexpected(yaddnsc::sdk::upstream_rejection(response, text));
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
