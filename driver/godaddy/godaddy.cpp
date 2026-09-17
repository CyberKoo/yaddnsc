//
// Created by Kotarou on 2026/7/13.
//

#include "godaddy.h"

#include <glaze/glaze.hpp>

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Services;
using yaddnsc::sdk::UpdateContext;

namespace {
    constexpr std::string_view API_URL = "https://api.godaddy.com/v1/domains/{DOMAIN}/records/{TYPE}/{NAME}";
    constexpr std::string_view DRIVER_NAME = "godaddy";

    /// GoDaddy DNS record update request body (single record in an array).
    struct GoDaddyRecordBody {
        std::string data;  ///< Record value (IP address)
        int ttl;           ///< Time-to-live in seconds
        std::string type;  ///< DNS record type (A, AAAA)
    };
} // anonymous namespace

template<>
struct glz::meta<GoDaddyRecordBody> {
    using T = GoDaddyRecordBody;
    static constexpr auto value = object(
        "data", &T::data,
        "ttl", &T::ttl,
        "type", &T::type
    );
};

YADDNSC_DEFINE_DRIVER(GoDaddyDriver, "godaddy", "Updates DNS records via the GoDaddy API", "Kotarou",
                      "1.0.0", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result GoDaddyDriver::update(UpdateContext &context) {
    const auto &params = context.request();
    const auto cfg = parse_config<GoDaddyParams>(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL,
                              fmt::arg("DOMAIN", params.domain),
                              fmt::arg("TYPE", params.record_type),
                              fmt::arg("NAME", params.subdomain));

    auto body = GoDaddyRecordBody{
        .data = std::string(params.ip_address),
        .ttl = cfg.ttl.value_or(600),
        .type = std::string(params.record_type)
    };

    // GoDaddy expects an array of records
    request.body = fmt::format("[{}]", glz::write_json(body).value_or("{}"));
    request.headers.push_back({"Authorization", fmt::format("sso-key {}:{}", cfg.key, cfg.secret)});
    request.content_type = "application/json";
    request.method = Method::Put;

    YADDNSC_SDK_LOG_DEBUG(context, "Domain {} ({}) received DNS record update request from driver {}, {}",
                          params.fqdn, params.record_type, DRIVER_NAME, yaddnsc::sdk::format_request(request));

    auto response = context.exchange(request);
    if (!response) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update failed (HTTP error: {})", params.fqdn,
                             params.record_type, response.error().message);
        return std::unexpected(Error{response.error().status, response.error().message, 0});
    }

    if (!check_response(*response, context.services())) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update rejected by upstream", params.fqdn, params.record_type);
        return std::unexpected(Error{YADDNSC_STATUS_UPSTREAM_REJECTED,
                                     fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn,
                                                 params.record_type),
                                     0});
    }

    return {};
}

bool GoDaddyDriver::check_response(const HttpResponse &response, const Services &services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    // GoDaddy returns 200 OK with an empty body on success.
    if (response.status_code == 200) {
        YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return true;
    }

    // Error responses typically include a JSON body with error details.
    if (!response.body.empty()) {
        YADDNSC_SDK_LOG_ERROR(services, "GoDaddy API error (HTTP {}): {}", response.status_code, response.body);
    } else {
        YADDNSC_SDK_LOG_ERROR(services, "GoDaddy API request failed with HTTP status {}", response.status_code);
    }

    return false;
}
