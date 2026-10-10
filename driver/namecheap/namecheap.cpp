#include "namecheap.h"

#include <libxml/parser.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlstring.h>
#include <libxml/xpath.h>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/xml_raii.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <expected>
#include <optional>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::optional member complete; clangd sees no spelled use
#include <vector>  // IWYU pragma: keep — sdk::HttpRequest aggregate init needs its std::vector member complete; clangd sees no spelled use

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
constexpr std::string_view API_URL = "https://dynamicdns.park-your-domain.com/update";
constexpr std::string_view DRIVER_NAME = "namecheap";
}  // namespace

YADDNSC_DEFINE_DRIVER(NamecheapDriver, "namecheap", "Updates DNS records via the Namecheap Dynamic DNS API", "Kotarou",
                      "1.0.0", YADDNSC_DRIVER_CAPABILITY_A)

// =============================================================================
//  NamecheapDriver::update
// =============================================================================

Result NamecheapDriver::validate(std::string_view driver_param_json) const {
    // Reuses the update-time schema: parse_config throws ConfigParseError on
    // missing keys or malformed values, which the ABI entry maps to
    // YADDNSC_STATUS_INVALID_CONFIG.
    [[maybe_unused]] const auto cfg = parse_config<NamecheapParams>(driver_param_json);
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

    const auto cfg = parse_config<NamecheapParams>(params.driver_param_json);

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
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    const auto doc = xml_raii::XmlDocument::parse(response.body);
    if (!doc) {
        YADDNSC_SDK_LOG_ERROR(services, "Failed to parse Namecheap API response XML");
        return false;
    }

    // Extract <ErrCount> — "0" means success.
    const auto count = doc->first_text("//ErrCount/text()");
    if (!count) {
        YADDNSC_SDK_LOG_ERROR(services, "Namecheap API response missing <ErrCount> element");
        return false;
    }

    if (*count == "0") {
        // Success — log the updated IP address from <IP>.
        const auto ip = doc->first_text("//IP/text()");
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully to {}", ip.value_or("unknown"));
        return true;
    }

    // Error — extract error messages from <errors> children.
    const auto messages = doc->all_text("//errors/*/text()");
    if (messages.empty()) {
        YADDNSC_SDK_LOG_ERROR(services, "Namecheap API error (ErrCount: {})", *count);
    }
    for (const auto& message : messages) {
        YADDNSC_SDK_LOG_ERROR(services, "Namecheap API error: {}", message);
    }
    return false;
}
