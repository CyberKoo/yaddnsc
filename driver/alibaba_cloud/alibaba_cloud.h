//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_ALIBABA_CLOUD_ALIBABA_CLOUD_H
#define YADDNSC_DRV_ALIBABA_CLOUD_ALIBABA_CLOUD_H

#include <string>

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

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
class AlibabaCloudDriver final : public yaddnsc::sdk::Driver {
public:
    ~AlibabaCloudDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

private:
    /// Build an Alibaba Cloud DNS UpdateDomainRecord request with RPC signature.
    static yaddnsc::sdk::HttpRequest generate_request(const AlibabaParams &cfg,
                                                      const yaddnsc::sdk::UpdateRequest &request);

    /// Validate the Alibaba Cloud DNS API response.
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif // YADDNSC_DRV_ALIBABA_CLOUD_ALIBABA_CLOUD_H
