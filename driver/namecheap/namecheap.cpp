#include "namecheap.h"

#include <expected>
#include <optional>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::optional member complete; clangd sees no spelled use
#include <string>
#include <string_view>
#include <vector>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::vector member complete; clangd sees no spelled use

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/log.hpp>
#include <yaddnsc/sdk/run_update.hpp>
#include <yaddnsc/sdk/upstream_failure.hpp>
#include <yaddnsc/sdk/xml_raii.hpp>

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

namespace {
constexpr std::string_view API_URL = "https://dynamicdns.park-your-domain.com/update";
constexpr std::string_view DRIVER_NAME = "namecheap";
/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "Namecheap API request";
}  // namespace

YADDNSC_DEFINE_ASYNC_DRIVER(NamecheapDriver, "namecheap", "Updates DNS records via the Namecheap Dynamic DNS API", "Kotarou",
                      "1.1.0", YADDNSC_DRIVER_CAPABILITY_A)

// =============================================================================
//  NamecheapDriver::update
// =============================================================================

NamecheapParams NamecheapDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<NamecheapParams>(json);
    require_non_empty("password", cfg.password);
    return cfg;
}

Result NamecheapDriver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
}

Task<Result> NamecheapDriver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();

    // Namecheap DDNS only supports A records.
    if (params.record_type == "AAAA") {
        return Task<Result>::from_value(std::unexpected(Error{YADDNSC_STATUS_UNSUPPORTED_RECORD,
                                                              fmt::format("Namecheap DDNS does not support AAAA "
                                                                          "(IPv6) records. Use an A record instead for "
                                                                          "domain '{}'.",
                                                                          params.fqdn),
                                                              0}));
    }

    const auto cfg = parse_params(params.driver_param_json);

    auto request = generate_request(cfg, params);

    return run_update(context, DRIVER_NAME, request, check_response);
}

// =============================================================================
//  NamecheapDriver::generate_request
// =============================================================================

HttpRequest NamecheapDriver::generate_request(const NamecheapParams& cfg, const UpdateRequest& params) {
    // Build URL:
    //   https://dynamicdns.park-your-domain.com/update
    //   ?host=HOST&domain=DOMAIN&password=PASS&ip=IP
    //
    // The `host` parameter uses the subdomain label directly.
    // For a bare-domain (apex) record the configuration should pass "@".
    HttpRequest request{};
    request.url = fmt::format("{}?host={}&domain={}&password={}&ip={}", API_URL, params.subdomain, params.domain,
                              cfg.password, params.ip_address);
    request.method = Method::GET;
    return request;
}

// =============================================================================
//  NamecheapDriver::check_response
// =============================================================================

Result NamecheapDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    const auto doc = xml_raii::XmlDocument::parse(response.body);

    // Extract <ErrCount> — "0" means success.
    const auto count = doc ? doc->first_text("//ErrCount/text()") : std::nullopt;
    if (count && *count == "0") {
        // Success — log the updated IP address from <IP>.
        const auto ip = doc->first_text("//IP/text()");
        YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully to {}", ip.value_or("unknown"));
        return {};
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    if (!doc) {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Failed to parse Namecheap API response XML");
        return std::unexpected(
            yaddnsc::sdk::upstream_rejection(response, "Failed to parse Namecheap API response XML"));
    }

    if (!count) {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Namecheap API response missing <ErrCount> element");
        return std::unexpected(
            yaddnsc::sdk::upstream_rejection(response, "Namecheap API response missing <ErrCount> element"));
    }

    // Error — extract error messages from <errors> children.
    const auto messages = doc->all_text("//errors/*/text()");
    if (messages.empty()) {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "Namecheap API returned ErrCount {} without an error message", *count);
    }
    std::string detail;
    for (const auto& message : messages) {
        const auto text = fmt::format("Namecheap API error: {}", message);
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "{}", text);
        if (!detail.empty()) {
            detail += "; ";
        }
        detail += text;
    }
    if (detail.empty()) {
        detail = fmt::format("Namecheap API returned ErrCount {} without an error message", *count);
    }
    return std::unexpected(yaddnsc::sdk::upstream_rejection(response, detail));
}
