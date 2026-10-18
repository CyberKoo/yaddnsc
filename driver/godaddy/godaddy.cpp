#include "godaddy.h"

#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/redact.hpp>

#include "config.hpp"

namespace glz {
template<class T>
struct meta;
}  // namespace glz

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::AsyncHttpResponse;
using yaddnsc::sdk::AsyncServices;
using yaddnsc::sdk::AsyncUpdateContext;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Task;
using yaddnsc::sdk::redact::truncate_for_log;

namespace {
constexpr std::string_view API_URL = "https://api.godaddy.com/v1/domains/{DOMAIN}/records/{TYPE}/{NAME}";
constexpr std::string_view DRIVER_NAME = "godaddy";

/// GoDaddy DNS record update request body (single record in an array).
struct GoDaddyRecordBody {
    std::string data;  ///< Record value (IP address)
    int ttl;           ///< Time-to-live in seconds
    std::string type;  ///< DNS record type (A, AAAA)
};
}  // anonymous namespace

template<>
struct glz::meta<GoDaddyRecordBody> {
    using T = GoDaddyRecordBody;
    static constexpr auto value = object("data", &T::data, "ttl", &T::ttl, "type", &T::type);
};

YADDNSC_DEFINE_ASYNC_DRIVER(GoDaddyDriver, "godaddy", "Updates DNS records via the GoDaddy API", "Kotarou", "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

GoDaddyParams GoDaddyDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<GoDaddyParams>(json);
    if (cfg.key.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'key' must not be empty");
    }
    if (cfg.secret.empty()) {
        throw yaddnsc::sdk::ConfigParseError("Required parameter 'secret' must not be empty");
    }
    if (cfg.ttl && *cfg.ttl < 0) {
        throw yaddnsc::sdk::ConfigParseError("Parameter 'ttl' must not be negative");
    }
    return cfg;
}

Result GoDaddyDriver::validate(std::string_view driver_param_json) const {
    [[maybe_unused]] const auto cfg = parse_params(driver_param_json);
    return {};
}

Task<Result> GoDaddyDriver::update(AsyncUpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_params(params.driver_param_json);

    HttpRequest request{};
    request.url = fmt::format(API_URL, fmt::arg("DOMAIN", params.domain), fmt::arg("TYPE", params.record_type),
                              fmt::arg("NAME", params.subdomain));

    auto body = GoDaddyRecordBody{
        .data = std::string(params.ip_address), .ttl = cfg.ttl.value_or(600), .type = std::string(params.record_type)};

    // GoDaddy expects an array of records
    request.body = fmt::format("[{}]", glz::write_json(body).value_or("{}"));
    request.headers.push_back({"Authorization", fmt::format("sso-key {}:{}", cfg.key, cfg.secret)});
    request.content_type = "application/json";
    request.method = Method::PUT;

    return run_update(context, DRIVER_NAME, request, check_response);
}

bool GoDaddyDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    YADDNSC_ASYNC_SDK_LOG_TRACE(services, "Got {} from server", truncate_for_log(response.body));

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (yaddnsc::sdk::detail::is_transient_status(response.status_code)) {
        YADDNSC_ASYNC_SDK_LOG_WARN(services, "GoDaddy API request failed with HTTP status {}: {}", response.status_code,
                             truncate_for_log(response.body));
        return false;
    }

    // GoDaddy returns 200 OK with an empty body on success.
    if (response.status_code == 200) {
        YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return true;
    }

    // Error responses typically include a JSON body with error details.
    if (!response.body.empty()) {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "GoDaddy API request failed with HTTP status {}: {}", response.status_code,
                              truncate_for_log(response.body));
    } else {
        YADDNSC_ASYNC_SDK_LOG_ERROR(services, "GoDaddy API request failed with HTTP status {}", response.status_code);
    }

    return false;
}
