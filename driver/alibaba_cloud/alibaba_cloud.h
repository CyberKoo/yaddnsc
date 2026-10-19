#ifndef YADDNSC_DRV_ALIBABA_CLOUD_ALIBABA_CLOUD_H
#define YADDNSC_DRV_ALIBABA_CLOUD_ALIBABA_CLOUD_H

#include <string_view>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_common.hpp>

struct AlibabaParams;

/// Alibaba Cloud DNS (Alidns) driver for updating A and AAAA records.
///
/// Implements the Alibaba Cloud DNS UpdateDomainRecord API using the
/// Alibaba Cloud RPC signing scheme (HMAC-SHA1) for authentication.
///
/// API reference:
///   https://www.alibabacloud.com/help/en/dns/api-alidns-2015-01-09-updatedomainrecord
///
/// Signing:
///   https://www.alibabacloud.com/help/en/sdk/request-signature
class AlibabaCloudDriver final : public yaddnsc::sdk::AsyncDriver {
public:
    ~AlibabaCloudDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    [[nodiscard]] yaddnsc::sdk::Task<yaddnsc::sdk::Result> update(yaddnsc::sdk::AsyncUpdateContext& context) override;

    /// Validate driver_param against the Alibaba Cloud API schema without
    /// updating; schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Shared schema and semantic validation for validate/update.
    [[nodiscard]] static AlibabaParams parse_params(std::string_view json);

    /// Build an Alibaba Cloud DNS UpdateDomainRecord request with RPC signature.
    static yaddnsc::sdk::HttpRequest generate_request(const AlibabaParams& cfg, const yaddnsc::sdk::UpdateRequest& ctx);

    /// Validate the Alibaba Cloud DNS API response. A rejected response
    /// reports the provider's own error detail.
    [[nodiscard]] static yaddnsc::sdk::Result check_response(const yaddnsc::sdk::AsyncHttpResponse& response,
                                                             const yaddnsc::sdk::AsyncServices& services);
};

#endif  // YADDNSC_DRV_ALIBABA_CLOUD_ALIBABA_CLOUD_H
