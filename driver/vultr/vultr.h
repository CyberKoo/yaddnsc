#ifndef YADDNSC_DRV_VULTR_VULTR_H
#define YADDNSC_DRV_VULTR_VULTR_H

#include <string_view>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_common.hpp>

struct VultrParams;

/// Vultr API v2 driver for DNS record updates.
///
/// Implements the Vultr DNS API for updating A and AAAA records
/// via their domain records endpoint.
///
/// API reference: https://www.vultr.com/api/#tag/dns
class VultrDriver final : public yaddnsc::sdk::AsyncDriver {
public:
    ~VultrDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    [[nodiscard]] yaddnsc::sdk::Task<yaddnsc::sdk::Result> update(yaddnsc::sdk::AsyncUpdateContext& context) override;

    /// Validate driver_param against the Vultr API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Shared schema and semantic validation for validate/update.
    [[nodiscard]] static VultrParams parse_params(std::string_view json);

    /// Validate the Vultr API response. A rejected response reports the
    /// provider's own error detail.
    [[nodiscard]] static yaddnsc::sdk::Result check_response(const yaddnsc::sdk::AsyncHttpResponse& response,
                                                             const yaddnsc::sdk::AsyncServices& services);
};

#endif  // YADDNSC_DRV_VULTR_VULTR_H
