//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_ALIBABA_CLOUD_ALIBABA_CLOUD_H
#define YADDNSC_DRV_ALIBABA_CLOUD_ALIBABA_CLOUD_H

#include "driver/base.h"

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
class AlibabaCloudDriver final : public BaseDriver {
public:
    ~AlibabaCloudDriver() override = default;

    /// Build an Alibaba Cloud DNS UpdateDomainRecord request with RPC signature.
    [[nodiscard]] DriverRequestContext generate_request(
        const DriverConfig &config, const DriverUpdateParams &ctx
    ) const override;

    /// Validate the Alibaba Cloud DNS API response.
    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;
};

#endif // YADDNSC_DRV_ALIBABA_CLOUD_ALIBABA_CLOUD_H
