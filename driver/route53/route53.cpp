//
// Created by Kotarou on 2026/7/13.
//

#include "route53.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "fmt.hpp"
#include "signing.h"
#include "config.hpp"
#include "driver/factory.h"
#include "interface/core_logger.h"

namespace {

    // ── Helpers ──────────────────────────────────────────────────────────────

    /// Convert std::string_view to std::span<const std::uint8_t> for the signing API.
    [[nodiscard]] std::span<const std::uint8_t> to_bytes(std::string_view sv) noexcept {
        return {reinterpret_cast<const std::uint8_t *>(sv.data()), sv.size()};
    }

    /// Ensure the FQDN has a trailing dot, as required by Route 53.
    [[nodiscard]] std::string ensure_trailing_dot(std::string_view fqdn) {
        if (fqdn.empty())
            return ".";
        if (fqdn.back() == '.')
            return std::string(fqdn);
        return fmt::format("{}.", fqdn);
    }

    /// The Route 53 XML namespace URI.
    constexpr std::string_view R53_XMLNS = "https://route53.amazonaws.com/doc/2013-04-01/";

    /// Route 53 API hostname.
    constexpr std::string_view R53_HOST = "route53.amazonaws.com";

    /// SigV4 algorithm identifier.
    constexpr std::string_view SIGV4_ALGORITHM = "AWS4-HMAC-SHA256";

    // ── SigV4 signing key derivation ────────────────────────────────────────

    /// Derive the multi-stage SigV4 signing key.
    [[nodiscard]] std::vector<std::uint8_t> derive_signing_key(
        std::string_view secret_access_key,
        std::string_view date_stamp,
        std::string_view region,
        std::string_view service,
        std::string_view request_type) {

        const auto k_secret = fmt::format("AWS4{}", secret_access_key);

        const auto k_date = Signing::hmac_sha256(to_bytes(k_secret), to_bytes(date_stamp));
        const auto k_region = Signing::hmac_sha256(k_date, to_bytes(region));
        const auto k_service = Signing::hmac_sha256(k_region, to_bytes(service));
        return Signing::hmac_sha256(k_service, to_bytes(request_type));
    }

} // anonymous namespace

// =============================================================================
//  Driver factory entry point
// =============================================================================

DEFINE_DRIVER_FACTORY(Route53Driver)

// =============================================================================
//  Route53Driver::generate_request
// =============================================================================

DriverRequestContext Route53Driver::generate_request(const DriverConfig &config,
                                                     const DriverUpdateParams &ctx) const {
    auto cfg = parse_config<Route53Params>(config);

    // Route 53 requires the FQDN with a trailing dot.
    auto fqdn = ensure_trailing_dot(ctx.fqdn);

    // Build the XML request body.
    auto ttl = cfg.ttl.value_or(300);
    auto body = build_xml_body(fqdn, ctx.rd_type, ctx.ip_addr, ttl);

    // URL path (used for both the request URL and the SigV4 canonical URI).
    constexpr std::string_view URI_PATH_PREFIX = "/2013-04-01/hostedzone/";
    auto url_path = fmt::format("{}{}/rrset", URI_PATH_PREFIX, cfg.hosted_zone_id);

    // -----------------------------------------------------------------------
    //  AWS SigV4 signing
    // -----------------------------------------------------------------------
    const auto amz_date = Signing::iso8601_timestamp();   // "YYYYMMDDTHHmmSSZ"
    const auto date_stamp = Signing::iso8601_date();      // "YYYYMMDD"
    const auto payload_hash = Signing::sha256_hex(body);

    // Signed headers (sorted alphabetically).
    constexpr std::string_view SIGNED_HEADERS = "host;x-amz-content-sha256;x-amz-date";
    auto canonical_headers = fmt::format("host:{}\nx-amz-content-sha256:{}\nx-amz-date:{}\n",
                                         R53_HOST, payload_hash, amz_date);

    // Build canonical request.
    auto canonical_request = fmt::format(
        "POST\n{}\n\n{}\n{}\n{}",
        url_path, canonical_headers, SIGNED_HEADERS, payload_hash);
    auto canonical_request_hash = Signing::sha256_hex(canonical_request);

    // Build credential scope and string-to-sign.
    auto credential_scope = fmt::format("{}/{}/route53/aws4_request",
                                        date_stamp, cfg.region);
    auto string_to_sign = fmt::format(
        "{}\n{}\n{}\n{}",
        SIGV4_ALGORITHM, amz_date, credential_scope, canonical_request_hash);

    // Derive signing key and compute signature.
    auto signing_key = derive_signing_key(cfg.secret_access_key, date_stamp,
                                          cfg.region, "route53", "aws4_request");
    auto signature_bytes = Signing::hmac_sha256(signing_key, to_bytes(string_to_sign));
    auto signature = Signing::hex_encode(signature_bytes);

    // Build the Authorization header.
    auto authorization = fmt::format(
        "{} Credential={}/{}, SignedHeaders={}, Signature={}",
        SIGV4_ALGORITHM, cfg.access_key_id, credential_scope,
        SIGNED_HEADERS, signature);

    // -----------------------------------------------------------------------
    //  Assemble the request
    // -----------------------------------------------------------------------
    auto url = fmt::format("https://{}{}", R53_HOST, url_path);

    DriverRequest request{};
    request.body = std::move(body);
    request.content_type = "application/xml";
    request.method = DriverHttpMethod::POST;
    request.headers.insert({"Host", std::string(R53_HOST)});
    request.headers.insert({"X-Amz-Date", std::move(amz_date)});
    request.headers.insert({"X-Amz-Content-SHA256", std::move(payload_hash)});
    request.headers.insert({"Authorization", std::move(authorization)});

    return {std::move(url), std::move(request)};
}

// =============================================================================
//  Route53Driver::check_response
// =============================================================================

bool Route53Driver::check_response(const HttpResponse &response) const {
    CORE_LOG_TRACE("Got {} from server.", response.body);

    if (response.status_code == 200) {
        // Route 53 returns HTTP 200 with <ChangeResourceRecordSetsResponse> on success.
        xmlDocPtr doc = xmlReadMemory(response.body.data(),
                                      static_cast<int>(response.body.size()),
                                      nullptr, nullptr, 0);
        if (!doc) {
            CORE_LOG_ERROR("Failed to parse Route 53 response XML");
            return false;
        }

        xmlXPathContextPtr xpath_ctx = xmlXPathNewContext(doc);
        if (!xpath_ctx) {
            xmlFreeDoc(doc);
            CORE_LOG_ERROR("Failed to create XPath context");
            return false;
        }

        // Register the Route 53 XML namespace.
        if (xmlXPathRegisterNs(xpath_ctx, BAD_CAST "r53",
                               BAD_CAST R53_XMLNS.data()) != 0) {
            xmlXPathFreeContext(xpath_ctx);
            xmlFreeDoc(doc);
            CORE_LOG_ERROR("Failed to register Route 53 XML namespace");
            return false;
        }

        // Extract <ChangeInfo><Status> text.
        constexpr const char *XPATH_STATUS =
            "//r53:ChangeResourceRecordSetsResponse/r53:ChangeInfo/r53:Status/text()";
        xmlXPathObjectPtr result = xmlXPathEvalExpression(
            BAD_CAST XPATH_STATUS, xpath_ctx);

        bool success = false;
        if (result && result->nodesetval && result->nodesetval->nodeNr > 0) {
            xmlChar *status_text = xmlNodeGetContent(
                result->nodesetval->nodeTab[0]);
            if (status_text) {
                std::string_view status(reinterpret_cast<const char *>(status_text));
                success = (status == "PENDING" || status == "INSYNC");
                if (success) {
                    CORE_LOG_DEBUG("DNS record updated successfully (status: {})", status);
                } else {
                    CORE_LOG_ERROR("Route 53 returned unexpected status: {}", status);
                }
                xmlFree(status_text);
            }
        } else {
            CORE_LOG_ERROR("Route 53 response missing <ChangeInfo><Status> element");
        }

        xmlXPathFreeObject(result);
        xmlXPathFreeContext(xpath_ctx);
        xmlFreeDoc(doc);
        return success;
    }

    // ── Error response: parse <ErrorResponse> XML ────────────────────────────
    if (!response.body.empty()) {
        xmlDocPtr doc = xmlReadMemory(response.body.data(),
                                      static_cast<int>(response.body.size()),
                                      nullptr, nullptr, 0);
        if (doc) {
            xmlXPathContextPtr xpath_ctx = xmlXPathNewContext(doc);
            if (xpath_ctx) {
                xmlXPathRegisterNs(xpath_ctx, BAD_CAST "r53",
                                   BAD_CAST R53_XMLNS.data());
                xmlXPathObjectPtr errors = xmlXPathEvalExpression(
                    BAD_CAST "//r53:Error", xpath_ctx);
                if (errors && errors->nodesetval) {
                    for (int i = 0; i < errors->nodesetval->nodeNr; ++i) {
                        xmlNodePtr error_node = errors->nodesetval->nodeTab[i];
                        xmlChar *code = nullptr;
                        xmlChar *msg = nullptr;
                        for (xmlNodePtr child = error_node->children;
                             child; child = child->next) {
                            if (child->type == XML_ELEMENT_NODE) {
                                if (xmlStrEqual(child->name, BAD_CAST "Code")) {
                                    code = xmlNodeGetContent(child);
                                } else if (xmlStrEqual(child->name, BAD_CAST "Message")) {
                                    msg = xmlNodeGetContent(child);
                                }
                            }
                        }
                        CORE_LOG_ERROR("Route 53 API error: {} ({})",
                                       msg ? reinterpret_cast<const char *>(msg) : "unknown",
                                       code ? reinterpret_cast<const char *>(code) : "no code");
                        xmlFree(code);
                        xmlFree(msg);
                    }
                } else {
                    CORE_LOG_ERROR("Route 53 API error (HTTP {}): {}",
                                   response.status_code, response.body);
                }
                xmlXPathFreeObject(errors);
                xmlXPathFreeContext(xpath_ctx);
            }
            xmlFreeDoc(doc);
        } else {
            CORE_LOG_ERROR("Route 53 API error (HTTP {}): {}",
                           response.status_code, response.body);
        }
    } else {
        CORE_LOG_ERROR("Route 53 API request failed with HTTP status {}",
                       response.status_code);
    }

    return false;
}

// =============================================================================
//  Route53Driver::get_detail
// =============================================================================

DriverDetail Route53Driver::get_detail() const noexcept {
    return {
        .name = "route53",
        .description = "Updates DNS records via the AWS Route 53 API",
        .author = "Kotarou",
        .version = "1.0.0"
    };
}

// =============================================================================
//  Route53Driver::build_xml_body
// =============================================================================

std::string Route53Driver::build_xml_body(const std::string &fqdn,
                                          std::string_view rd_type,
                                          std::string_view ip_addr,
                                          int ttl) {
    // Build the UPSERT XML document using libxml2's tree API.
    // This ensures proper XML escaping, namespace handling, and encoding.
    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    if (!doc)
        return {};

    xmlNodePtr root = xmlNewDocNode(doc, nullptr, BAD_CAST "ChangeResourceRecordSetsRequest", nullptr);
    if (!root) {
        xmlFreeDoc(doc);
        return {};
    }
    xmlDocSetRootElement(doc, root);

    // Register the default namespace on the root element.
    xmlNsPtr ns = xmlNewNs(root, BAD_CAST R53_XMLNS.data(), nullptr);
    if (!ns) {
        xmlFreeDoc(doc);
        return {};
    }
    xmlSetNs(root, ns);

    // Build the nested element hierarchy.
    xmlNodePtr batch = xmlNewChild(root, ns, BAD_CAST "ChangeBatch", nullptr);
    xmlNodePtr changes = xmlNewChild(batch, ns, BAD_CAST "Changes", nullptr);
    xmlNodePtr change = xmlNewChild(changes, ns, BAD_CAST "Change", nullptr);

    xmlNewTextChild(change, ns, BAD_CAST "Action", BAD_CAST "UPSERT");

    xmlNodePtr rrset = xmlNewChild(change, ns, BAD_CAST "ResourceRecordSet", nullptr);
    xmlNewTextChild(rrset, ns, BAD_CAST "Name", BAD_CAST fqdn.data());
    xmlNewTextChild(rrset, ns, BAD_CAST "Type", BAD_CAST rd_type.data());

    auto ttl_str = std::to_string(ttl);
    xmlNewTextChild(rrset, ns, BAD_CAST "TTL", BAD_CAST ttl_str.data());

    xmlNodePtr records = xmlNewChild(rrset, ns, BAD_CAST "ResourceRecords", nullptr);
    xmlNodePtr record = xmlNewChild(records, ns, BAD_CAST "ResourceRecord", nullptr);
    xmlNewTextChild(record, ns, BAD_CAST "Value", BAD_CAST ip_addr.data());

    // Serialise the document to a string.
    xmlChar *xml_buf = nullptr;
    int xml_len = 0;
    xmlDocDumpMemory(doc, &xml_buf, &xml_len);
    xmlFreeDoc(doc);

    if (!xml_buf)
        return {};

    std::string result(reinterpret_cast<const char *>(xml_buf),
                       static_cast<std::size_t>(xml_len));
    xmlFree(xml_buf);
    return result;
}
