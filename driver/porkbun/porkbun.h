#ifndef YADDNSC_DRV_PORKBUN_PORKBUN_H
#define YADDNSC_DRV_PORKBUN_PORKBUN_H

#include <string>
#include <string_view>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_common.hpp>

struct PorkbunParams;

/// Porkbun API v3 driver for DNS record updates.
///
/// Implements the Porkbun DNS API for updating A and AAAA records
/// via their edit by name and type endpoint.
///
/// API reference: https://porkbun.com/api/json/v3/documentation
class PorkbunDriver final : public yaddnsc::sdk::AsyncDriver {
public:
    ~PorkbunDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    [[nodiscard]] yaddnsc::sdk::Task<yaddnsc::sdk::Result> update(yaddnsc::sdk::AsyncUpdateContext& context) override;

    /// Validate driver_param against the Porkbun API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Shared schema and semantic validation for validate/update.
    [[nodiscard]] static PorkbunParams parse_params(std::string_view json);

    /// Build the JSON request body for a Porkbun DNS record update.
    static std::string generate_body(const PorkbunParams& cfg, const yaddnsc::sdk::UpdateRequest& request);

    /// Validate the Porkbun API response. A rejected response reports the
    /// provider's own error detail.
    [[nodiscard]] static yaddnsc::sdk::Result check_response(const yaddnsc::sdk::AsyncHttpResponse& response,
                                                             const yaddnsc::sdk::AsyncServices& services);
};

#endif  // YADDNSC_DRV_PORKBUN_PORKBUN_H
