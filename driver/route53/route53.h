//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_ROUTE53_ROUTE53_H
#define YADDNSC_DRV_ROUTE53_ROUTE53_H

#include <string>
#include <string_view>

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// AWS Route 53 DNS driver for updating A and AAAA records.
///
/// Implements the Route 53 ChangeResourceRecordSets API using AWS SigV4
/// request signing for authentication.  Request bodies are XML and responses
/// are parsed via libxml2.
///
/// API reference:
///   https://docs.aws.amazon.com/Route53/latest/APIReference/API_ChangeResourceRecordSets.html
///
/// Authentication:
///   https://docs.aws.amazon.com/general/latest/gr/sigv4_signing.html
class Route53Driver final : public yaddnsc::sdk::Driver {
public:
    ~Route53Driver() override = default;

    /// Perform one update: build signed request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

    /// Validate driver_param against the Route53 API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Validate the Route 53 API response (XML with libxml2).
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);

    /// Build the XML request body for a Route 53 UPSERT change batch.
    static std::string build_xml_body(const std::string &fqdn,
                                      std::string_view rd_type,
                                      std::string_view ip_addr,
                                      int ttl);
};

#endif // YADDNSC_DRV_ROUTE53_ROUTE53_H
