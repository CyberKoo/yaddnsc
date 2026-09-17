//
// Created by Kotarou on 2026/7/13.
//

#include "namecheap.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

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

namespace {
    constexpr std::string_view API_URL = "https://dynamicdns.park-your-domain.com/update";
    constexpr std::string_view DRIVER_NAME = "namecheap";
}

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

Result NamecheapDriver::update(UpdateContext &context) {
    const auto &params = context.request();

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

    return run_update(context, DRIVER_NAME, request,
                      [](const HttpResponse &response, const Services &services) {
                          return check_response(response, services);
                      });
}

// =============================================================================
//  NamecheapDriver::generate_request
// =============================================================================

HttpRequest NamecheapDriver::generate_request(const NamecheapParams &cfg, const UpdateRequest &params) {
    // Build URL:
    //   https://dynamicdns.park-your-domain.com/update
    //   ?host=HOST&domain=DOMAIN&password=PASS&ip=IP
    //
    // The `host` parameter uses the subdomain label directly.
    // For a bare-domain (apex) record the configuration should pass "@".
    HttpRequest request{};
    request.url = fmt::format("{}?host={}&domain={}&password={}&ip={}", API_URL, params.subdomain, params.domain,
                              cfg.password, params.ip_address);
    request.method = Method::Get;
    return request;
}

// =============================================================================
//  NamecheapDriver::check_response
// =============================================================================

bool NamecheapDriver::check_response(const HttpResponse &response, const Services &services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    // Parse the XML response with libxml2.
    // All libxml2 resources are RAII-managed via xml_raii wrappers.
    xml_raii::unique_doc doc(
        xmlReadMemory(response.body.data(), static_cast<int>(response.body.size()), nullptr, nullptr, 0));

    if (!doc) {
        YADDNSC_SDK_LOG_ERROR(services, "Failed to parse Namecheap API response XML");
        return false;
    }

    xml_raii::unique_xpath_ctx xpath_ctx(xmlXPathNewContext(doc.get()));
    if (!xpath_ctx) {
        YADDNSC_SDK_LOG_ERROR(services, "Failed to create XPath context");
        return false;
    }

    // Extract <ErrCount> — "0" means success.
    xml_raii::unique_xpath_obj err_count_nodes(xmlXPathEvalExpression(BAD_CAST "//ErrCount/text()", xpath_ctx.get()));

    bool success = false;

    if (err_count_nodes && err_count_nodes->nodesetval && err_count_nodes->nodesetval->nodeNr > 0) {
        xmlChar* count_text = xmlNodeGetContent(err_count_nodes->nodesetval->nodeTab[0]);

        if (count_text) {
            std::string_view count(reinterpret_cast<const char*>(count_text));

            if (count == "0") {
                // Success — log the updated IP address from <IP>.
                xml_raii::unique_xpath_obj ip_nodes(xmlXPathEvalExpression(BAD_CAST "//IP/text()", xpath_ctx.get()));
                if (ip_nodes && ip_nodes->nodesetval && ip_nodes->nodesetval->nodeNr > 0) {
                    xmlChar* ip_text = xmlNodeGetContent(ip_nodes->nodesetval->nodeTab[0]);
                    YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully to {}",
                                          ip_text ? reinterpret_cast<const char*>(ip_text) : "unknown");
                    xmlFree(ip_text);
                }
                success = true;

            } else {
                // Error — extract error messages from <errors> children.
                xml_raii::unique_xpath_obj err_msg_nodes(
                    xmlXPathEvalExpression(BAD_CAST "//errors/*/text()", xpath_ctx.get()));
                if (err_msg_nodes && err_msg_nodes->nodesetval) {
                    for (int i = 0; i < err_msg_nodes->nodesetval->nodeNr; ++i) {
                        xmlChar* err_text = xmlNodeGetContent(err_msg_nodes->nodesetval->nodeTab[i]);
                        YADDNSC_SDK_LOG_ERROR(services, "Namecheap API error: {}",
                                              err_text ? reinterpret_cast<const char*>(err_text) : "unknown");
                        xmlFree(err_text);
                    }
                } else {
                    YADDNSC_SDK_LOG_ERROR(services, "Namecheap API error (ErrCount: {})", count);
                }
            }

            xmlFree(count_text);
        }
    } else {
        YADDNSC_SDK_LOG_ERROR(services, "Namecheap API response missing <ErrCount> element");
    }

    return success;
}
