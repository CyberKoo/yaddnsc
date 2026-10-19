#include "godaddy.h"

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/async_driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/log.hpp>
#include <yaddnsc/sdk/run_update.hpp>
#include <yaddnsc/sdk/upstream_failure.hpp>

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

namespace {
constexpr std::string_view API_URL = "https://api.godaddy.com/v1/domains/{DOMAIN}/records/{TYPE}/{NAME}";
constexpr std::string_view DRIVER_NAME = "godaddy";
/// Subject of the shared rejection messages.
constexpr std::string_view REJECTION_SUBJECT = "GoDaddy API request";

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

YADDNSC_DEFINE_ASYNC_DRIVER(GoDaddyDriver, "godaddy", "Updates DNS records via the GoDaddy API", "Kotarou", "1.1.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

GoDaddyParams GoDaddyDriver::parse_params(std::string_view json) {
    auto cfg = parse_config<GoDaddyParams>(json);
    require_non_empty("key", cfg.key);
    require_non_empty("secret", cfg.secret);
    optional_non_negative("ttl", cfg.ttl);
    return cfg;
}

Result GoDaddyDriver::validate(std::string_view driver_param_json) const {
    return validate_via(driver_param_json, parse_params);
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

Result GoDaddyDriver::check_response(const AsyncHttpResponse& response, const AsyncServices& services) {
    yaddnsc::sdk::log_response_body(services, response);

    // 429 and 5xx are retried by the next update cycle; the rest needs operator action.
    if (auto rejection = yaddnsc::sdk::transient_rejection(services, response, REJECTION_SUBJECT)) {
        return std::unexpected(*rejection);
    }

    // GoDaddy returns 200 OK with an empty body on success.
    if (response.status_code == 200) {
        YADDNSC_ASYNC_SDK_LOG_DEBUG(services, "DNS record updated successfully");
        return {};
    }

    // Error responses typically include a JSON body with error details.
    return std::unexpected(yaddnsc::sdk::status_rejection(services, response, REJECTION_SUBJECT));
}
