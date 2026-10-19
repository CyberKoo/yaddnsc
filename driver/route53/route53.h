#ifndef YADDNSC_DRV_ROUTE53_ROUTE53_H
#define YADDNSC_DRV_ROUTE53_ROUTE53_H

#include <string_view>

#include <yaddnsc/sdk/async_driver.hpp>

struct Route53Params;

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
class Route53Driver final : public yaddnsc::sdk::AsyncDriver {
public:
    ~Route53Driver() override = default;

    /// Perform one update: build signed request → HTTP exchange → check-response.
    [[nodiscard]] yaddnsc::sdk::Task<yaddnsc::sdk::Result> update(yaddnsc::sdk::AsyncUpdateContext& context) override;

    /// Validate driver_param against the Route53 API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Shared schema and semantic validation for validate/update.
    [[nodiscard]] static Route53Params parse_params(std::string_view json);

    /// Validate the Route 53 API response (XML with libxml2). A rejected
    /// response reports the provider's own error detail.
    [[nodiscard]] static yaddnsc::sdk::Result check_response(const yaddnsc::sdk::AsyncHttpResponse& response,
                                                             const yaddnsc::sdk::AsyncServices& services);
};

#endif  // YADDNSC_DRV_ROUTE53_ROUTE53_H
