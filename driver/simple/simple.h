//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_SIMPLE_SIMPLE_H
#define YADDNSC_DRV_SIMPLE_SIMPLE_H

#include <glaze/json/generic_fwd.hpp>
#include <yaddnsc/sdk/driver.hpp>
#include <string_view>

/// Simple HTTP GET driver for DNS record updates.
///
/// The simplest driver implementation — it embeds the IP address into
/// the URL or request body and doesn't parse the response beyond checking
/// the HTTP status code.
class SimpleDriver final : public yaddnsc::sdk::Driver {
public:
    ~SimpleDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext& context) override;

    /// Validate driver_param without updating; requires a string "url"
    /// member — the same check the update path performs.
    yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Parse driver_param and require a string "url" member. Shared by the
    /// update and validate paths; throws ConfigParseError on violations.
    [[nodiscard]] static glz::generic parse_driver_param(std::string_view driver_param_json);

    /// Build the HTTP GET request with the IP address embedded in the URL template.
    static yaddnsc::sdk::HttpRequest generate_request(const yaddnsc::sdk::UpdateRequest& params);

    /// Validate the response — returns true for 2xx status codes.
    static bool check_response(const yaddnsc::sdk::HttpResponse& response, const yaddnsc::sdk::Services& services);
};

#endif  // YADDNSC_DRV_SIMPLE_SIMPLE_H
