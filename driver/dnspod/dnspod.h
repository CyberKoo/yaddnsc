#ifndef YADDNSC_DRV_DNSPOD_DNSPOD_H
#define YADDNSC_DRV_DNSPOD_DNSPOD_H

#include <string_view>

#include <yaddnsc/sdk/async_driver.hpp>

struct DnsPodParams;

/// DNSPod API driver for DNS record updates.
///
/// Implements the DNSPod API for updating DNS records via their
/// Record.Modify endpoint.
class DnsPodDriver final : public yaddnsc::sdk::AsyncDriver {
public:
    ~DnsPodDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    [[nodiscard]] yaddnsc::sdk::Task<yaddnsc::sdk::Result> update(yaddnsc::sdk::AsyncUpdateContext& context) override;

    /// Validate driver_param against the DNSPod API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Shared schema and semantic validation for validate/update.
    [[nodiscard]] static DnsPodParams parse_params(std::string_view json);

    /// Build the DNSPod API request from parsed config and update params.
    [[nodiscard]] static yaddnsc::sdk::HttpRequest generate_request(const DnsPodParams& cfg,
                                                                    const yaddnsc::sdk::UpdateRequest& params);

    /// Validate the DNSPod API response. A rejected response reports the
    /// provider's own error detail.
    [[nodiscard]] static yaddnsc::sdk::Result check_response(const yaddnsc::sdk::AsyncHttpResponse& response,
                                                             const yaddnsc::sdk::AsyncServices& services);

    /// Convert a DNSPod error code to a human-readable description.
    [[nodiscard]] static std::string_view describe_error_code(std::string_view code);
};

#endif  // YADDNSC_DRV_DNSPOD_DNSPOD_H
