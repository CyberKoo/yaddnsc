#ifndef YADDNSC_DRV_DIGITAL_OCEAN_DIGITAL_OCEAN_H
#define YADDNSC_DRV_DIGITAL_OCEAN_DIGITAL_OCEAN_H

#include <string_view>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_common.hpp>

struct DigitalOceanParams;

/// DigitalOcean API driver for DNS record updates.
///
/// Implements the DigitalOcean API v2 for updating DNS records
/// via their Domain Records endpoint.
class DigitalOceanDriver final : public yaddnsc::sdk::AsyncDriver {
public:
    ~DigitalOceanDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    [[nodiscard]] yaddnsc::sdk::Task<yaddnsc::sdk::Result> update(yaddnsc::sdk::AsyncUpdateContext& context) override;

    /// Validate driver_param against the DigitalOcean API schema without
    /// updating; schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Shared schema and semantic validation for validate/update.
    [[nodiscard]] static DigitalOceanParams parse_params(std::string_view json);

    /// Validate the DigitalOcean API response. A rejected response reports
    /// the provider's own error detail.
    [[nodiscard]] static yaddnsc::sdk::Result check_response(const yaddnsc::sdk::AsyncHttpResponse& response,
                                                             const yaddnsc::sdk::AsyncServices& services);
};

#endif  // YADDNSC_DRV_DIGITAL_OCEAN_DIGITAL_OCEAN_H
