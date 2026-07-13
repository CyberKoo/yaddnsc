//
// Created by Kotarou on 2026/7/13.
//

#include "namecheap.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#include "driver/factory.h"
#include "driver/xml_raii.hpp"
#include "interface/core_logger.h"

#include "config.hpp"
#include "fmt.hpp"

namespace {
constexpr std::string_view API_URL = "https://dynamicdns.park-your-domain.com/update";
}  // anonymous namespace

DEFINE_DRIVER_FACTORY(NamecheapDriver)

// =============================================================================
//  NamecheapDriver::generate_request
// =============================================================================

DriverRequestContext NamecheapDriver::generate_request(const DriverConfig& config,
                                                       const DriverUpdateParams& ctx) const {
    // Namecheap DDNS only supports A records.
    if (ctx.rd_type == "AAAA") {
        throw ParamParseException(
            fmt::format("Namecheap DDNS does not support AAAA (IPv6) records. "
                        "Use an A record instead for domain '{}'.",
                        ctx.fqdn));
    }

    auto cfg = parse_config<NamecheapParams>(config);

    // Build URL:
    //   https://dynamicdns.park-your-domain.com/update
    //   ?host=HOST&domain=DOMAIN&password=PASS&ip=IP
    //
    // The `host` parameter uses the subdomain label directly.
    // For a bare-domain (apex) record the configuration should pass "@".
    auto url = fmt::format("{}?host={}&domain={}&password={}&ip={}", API_URL, ctx.subdomain, ctx.domain, cfg.password,
                           ctx.ip_addr);

    DriverRequest request{};
    request.method = DriverHttpMethod::GET;

    return {std::move(url), std::move(request)};
}

// =============================================================================
//  NamecheapDriver::check_response
// =============================================================================

bool NamecheapDriver::check_response(const HttpResponse& response) const {
    CORE_LOG_TRACE("Got {} from server.", response.body);

    // Parse the XML response with libxml2.
    // All libxml2 resources are RAII-managed via xml_raii wrappers.
    xml_raii::unique_doc doc(
        xmlReadMemory(response.body.data(), static_cast<int>(response.body.size()), nullptr, nullptr, 0));

    if (!doc) {
        CORE_LOG_ERROR("Failed to parse Namecheap API response XML");
        return false;
    }

    xml_raii::unique_xpath_ctx xpath_ctx(xmlXPathNewContext(doc.get()));
    if (!xpath_ctx) {
        CORE_LOG_ERROR("Failed to create XPath context");
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
                    CORE_LOG_DEBUG("DNS record updated successfully to {}",
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
                        CORE_LOG_ERROR("Namecheap API error: {}",
                                       err_text ? reinterpret_cast<const char*>(err_text) : "unknown");
                        xmlFree(err_text);
                    }
                } else {
                    CORE_LOG_ERROR("Namecheap API error (ErrCount: {})", count);
                }
            }

            xmlFree(count_text);
        }
    } else {
        CORE_LOG_ERROR("Namecheap API response missing <ErrCount> element");
    }

    return success;
}

// =============================================================================
//  NamecheapDriver::get_detail
// =============================================================================

DriverDetail NamecheapDriver::get_detail() const noexcept {
    return {.name = "namecheap",
            .description = "Updates DNS records via the Namecheap Dynamic DNS API",
            .author = "Kotarou",
            .version = "1.0.0"};
}
