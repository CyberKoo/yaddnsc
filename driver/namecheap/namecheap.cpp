#include "namecheap.h"

#include <optional>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::optional member complete; clangd sees no spelled use
#include <string>
#include <string_view>
#include <vector>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::vector member complete; clangd sees no spelled use

#include <expected>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/redact.hpp>
#include <yaddnsc/sdk/xml_raii.hpp>

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
constexpr std::string_view API_URL = "https://dynamicdns.park-your-domain.com/update";
constexpr std::string_view DRIVER_NAME = "namecheap";
}  // namespace

YADDNSC_DEFINE_DRIVER(NamecheapDriver, "namecheap", "Updates DNS records via the Namecheap Dynamic DNS API", "Kotarou",
                      "1.0.0", YADDNSC_DRIVER_CAPABILITY_A, 0)

// =============================================================================
//  NamecheapDriver::update
// =============================================================================

NamecheapParams NamecheapDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<NamecheapParams>(json);
    if (cfg.password.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'password' must not be empty");
    }
    return cfg;
}

Result NamecheapDriver::validate(std::string_view driver_param_json) const {
    [[maybe_unused]] const auto cfg = parse_params(driver_param_json);
    return {};
}

Result NamecheapDriver::update(UpdateContext& context) {
    const auto& params = context.request();

    // Namecheap DDNS only supports A records.
    if (params.record_type == "AAAA") {
        return std::unexpected(Error{YADDNSC_STATUS_UNSUPPORTED_RECORD,
                                     fmt::format("Namecheap DDNS does not support AAAA (IPv6) records. "
                                                 "Use an A record instead for domain '{}'.",
                                                 params.fqdn),
                                     0});
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

bool NamecheapDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server", truncate_for_log(response.body));

    const auto doc = xml_raii::XmlDocument::parse(response.body);

    // Extract <ErrCount> — "0" means success.
    const auto count = doc ? doc->first_text("//ErrCount/text()") : std::nullopt;
    if (count && *count == "0") {
        // Success — log the updated IP address from <IP>.
        const auto ip = doc->first_text("//IP/text()");
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully to {}", ip.value_or("unknown"));
        return true;
    }

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (yaddnsc::sdk::detail::is_transient_status(response.status_code)) {
        YADDNSC_SDK_LOG_WARN(services, "Namecheap API request failed with HTTP status {}: {}", response.status_code,
                             truncate_for_log(response.body));
        return false;
    }

    if (!doc) {
        YADDNSC_SDK_LOG_ERROR(services, "Failed to parse Namecheap API response XML");
        return false;
    }

    if (!count) {
        YADDNSC_SDK_LOG_ERROR(services, "Namecheap API response missing <ErrCount> element");
        return false;
    }

    // Error — extract error messages from <errors> children.
    const auto messages = doc->all_text("//errors/*/text()");
    if (messages.empty()) {
        YADDNSC_SDK_LOG_ERROR(services, "Namecheap API returned ErrCount {} without an error message", *count);
    }
    for (const auto& message : messages) {
        YADDNSC_SDK_LOG_ERROR(services, "Namecheap API error: {}", message);
    }
    return false;
}
