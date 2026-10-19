#ifndef YADDNSC_DRV_GODADDY_GODADDY_H
#define YADDNSC_DRV_GODADDY_GODADDY_H

#include <string_view>

#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_common.hpp>

struct GoDaddyParams;

/// GoDaddy API driver for DNS record updates.
///
/// Implements the GoDaddy Domains API v1 for updating DNS records
/// via their record replacement endpoint.
///
/// API reference: https://developer.godaddy.com/doc/endpoint/domains
class GoDaddyDriver final : public yaddnsc::sdk::AsyncDriver {
public:
    ~GoDaddyDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    [[nodiscard]] yaddnsc::sdk::Task<yaddnsc::sdk::Result> update(yaddnsc::sdk::AsyncUpdateContext& context) override;

    /// Validate driver_param against the GoDaddy API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Shared schema and semantic validation for validate/update.
    [[nodiscard]] static GoDaddyParams parse_params(std::string_view json);

    /// Validate the GoDaddy API response. A rejected response names the HTTP
    /// status the provider answered with.
    [[nodiscard]] static yaddnsc::sdk::Result check_response(const yaddnsc::sdk::AsyncHttpResponse& response,
                                                             const yaddnsc::sdk::AsyncServices& services);
};

#endif  // YADDNSC_DRV_GODADDY_GODADDY_H
