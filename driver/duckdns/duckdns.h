#ifndef YADDNSC_DRV_DUCKDNS_DUCKDNS_H
#define YADDNSC_DRV_DUCKDNS_DUCKDNS_H

#include <string>
#include <string_view>

#include <yaddnsc/sdk/async_driver.hpp>

struct DuckDnsParams;

/// DuckDNS API driver for DDNS record updates.
///
/// Implements the DuckDNS HTTP API for updating A and AAAA records
/// via their simple GET-based update endpoint.
///
/// API reference: https://www.duckdns.org/spec.jsp
class DuckDnsDriver final : public yaddnsc::sdk::AsyncDriver {
public:
    ~DuckDnsDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    [[nodiscard]] yaddnsc::sdk::Task<yaddnsc::sdk::Result> update(yaddnsc::sdk::AsyncUpdateContext& context) override;

    /// Validate driver_param against the DuckDNS API schema without updating;
    /// schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Shared schema and semantic validation for validate/update.
    [[nodiscard]] static DuckDnsParams parse_params(std::string_view json);

    /// Build the DuckDNS API request URL from config and update params.
    static std::string generate_url(const DuckDnsParams& cfg, const yaddnsc::sdk::UpdateRequest& params);

    /// Validate the DuckDNS API response (expects "OK" or "KO"). A rejected
    /// response reports the provider's own message.
    [[nodiscard]] static yaddnsc::sdk::Result check_response(const yaddnsc::sdk::AsyncHttpResponse& response,
                                                             const yaddnsc::sdk::AsyncServices& services);
};

#endif  // YADDNSC_DRV_DUCKDNS_DUCKDNS_H
