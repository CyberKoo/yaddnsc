//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_ROUTE53_ROUTE53_H
#define YADDNSC_DRV_ROUTE53_ROUTE53_H

#include "driver/base.h"

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
class Route53Driver final : public BaseDriver {
public:
    ~Route53Driver() override = default;

    /// Build a Route 53 ChangeResourceRecordSets request with SigV4 headers.
    [[nodiscard]] DriverRequestContext generate_request(
        const DriverConfig &config, const DriverUpdateParams &ctx
    ) const override;

    /// Validate the Route 53 API response (XML with libxml2).
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;

private:
    /// Build the XML request body for a Route 53 UPSERT change batch.
    static std::string build_xml_body(const std::string &fqdn,
                                      std::string_view rd_type,
                                      std::string_view ip_addr,
                                      int ttl);
};

#endif // YADDNSC_DRV_ROUTE53_ROUTE53_H
