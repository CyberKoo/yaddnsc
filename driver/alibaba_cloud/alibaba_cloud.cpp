//
// Created by Kotarou on 2026/7/13.
//

#include "alibaba_cloud.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <ctime>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/url_encode.hpp>
#include <yaddnsc/util/format.hpp>
#include <yaddnsc/util/url_encode.hpp>

#include "config.hpp"
#include "response.hpp"
#include "signing.h"

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Services;
using yaddnsc::sdk::UpdateContext;

namespace {

// ── Helpers ──────────────────────────────────────────────────────────────

/// Alibaba Cloud API endpoint.
constexpr std::string_view API_URL = "https://alidns.aliyuncs.com/";

/// API version for Alidns.
constexpr std::string_view API_VERSION = "2015-01-09";

/// Driver name for request logging.
constexpr std::string_view DRIVER_NAME = "alibaba_cloud";

/// Generate an ISO 8601 timestamp in Alibaba Cloud format: "YYYY-MM-DDTHH:MM:SSZ".
[[nodiscard]] std::string alibaba_timestamp() noexcept {
    const auto now = std::time(nullptr);
    const auto* tm = std::gmtime(&now);
    if (!tm)
        return {};
    std::array<char, 24> buf{};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", tm);
    return {buf.data()};
}

/// Generate a unique SignatureNonce for each request.
[[nodiscard]] std::string generate_nonce() {
    // Combine high-resolution clock with a random component for uniqueness.
    auto now = std::chrono::high_resolution_clock::now();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937_64 gen(rd());
    auto extra = gen();

    return fmt::format("{:020}{:020}", nanos, extra);
}

// ── Alibaba Cloud RPC signature ──────────────────────────────────────────

/// Sort the parameter map by key (lexicographic order as required by Alibaba).
using ParamList = std::vector<std::pair<std::string, std::string>>;

/// Build the canonical query string: URL-encoded key=value pairs sorted by key.
[[nodiscard]] std::string build_canonical_query(const ParamList& params) {
    std::string result;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!first)
            result += '&';
        result += yaddnsc::sdk::url_encode(key);
        result += '=';
        result += yaddnsc::sdk::url_encode(value);
        first = false;
    }
    return result;
}

/// Compute the Alibaba Cloud RPC signature string.
///
/// StringToSign = HTTPMethod + "&" + percentEncode("/") + "&" +
///                percentEncode(canonical_query)
///
/// Signature = Base64(HMAC-SHA1(StringToSign, SecretAccessKey + "&"))
[[nodiscard]] std::string compute_signature(std::string_view secret_access_key,
                                            std::string_view http_method,
                                            const std::string& canonical_query) {
    // Build string to sign.
    auto encoded_path = yaddnsc::sdk::url_encode("/");
    auto encoded_query = yaddnsc::sdk::url_encode(canonical_query);
    auto string_to_sign = fmt::format("{}&{}&{}", http_method, encoded_path, encoded_query);

    // HMAC-SHA1 with key = secret + "&".
    auto key_str = fmt::format("{}&", secret_access_key);
    auto hmac_result = Signing::hmac_sha1(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(key_str.data()), key_str.size()),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(string_to_sign.data()),
                                      string_to_sign.size()));

    // Base64 encode and return.
    return Signing::base64_encode(hmac_result);
}

}  // anonymous namespace

YADDNSC_DEFINE_DRIVER(AlibabaCloudDriver,
                      "alibaba_cloud",
                      "Updates DNS records via the Alibaba Cloud DNS API",
                      "Kotarou",
                      "1.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result AlibabaCloudDriver::validate(std::string_view driver_param_json) const {
    // Reuses the update-time schema: parse_config throws ConfigParseError on
    // missing keys or malformed values, which the ABI entry maps to
    // YADDNSC_STATUS_INVALID_CONFIG.
    [[maybe_unused]] const auto cfg = parse_config<AlibabaParams>(driver_param_json);
    return {};
}

Result AlibabaCloudDriver::update(UpdateContext& context) {
    const auto& params = context.request();
    const auto cfg = parse_config<AlibabaParams>(params.driver_param_json);

    HttpRequest request = generate_request(cfg, params);

    return run_update(context, DRIVER_NAME, request, [](const HttpResponse& response, const Services& services) {
        return check_response(response, services);
    });
}

HttpRequest AlibabaCloudDriver::generate_request(const AlibabaParams& cfg, const yaddnsc::sdk::UpdateRequest& ctx) {
    auto ttl = cfg.ttl.value_or(600);

    // Build the common parameters (all RPC requests include these).
    ParamList params;
    params.emplace_back("Action", "UpdateDomainRecord");
    params.emplace_back("Format", "JSON");
    params.emplace_back("Version", std::string(API_VERSION));
    params.emplace_back("AccessKeyId", cfg.access_key_id);
    params.emplace_back("SignatureMethod", "HMAC-SHA1");
    params.emplace_back("SignatureVersion", "1.0");
    params.emplace_back("SignatureNonce", generate_nonce());
    params.emplace_back("Timestamp", alibaba_timestamp());

    // UpdateDomainRecord specific parameters.
    params.emplace_back("RecordId", cfg.record_id);
    params.emplace_back("RR", ctx.subdomain);
    params.emplace_back("Type", ctx.record_type);
    params.emplace_back("Value", ctx.ip_address);
    params.emplace_back("TTL", std::to_string(ttl));

    // Sort by key name (required for Alibaba Cloud RPC signature).
    std::sort(params.begin(), params.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    // Build the canonical query string and compute the signature.
    auto canonical_query = build_canonical_query(params);

    // Note: Alibaba Cloud uses "POST" as the HTTP method in the string to sign,
    // even though the actual query is sent as form-encoded body.
    auto signature = compute_signature(cfg.access_key_secret, "POST", canonical_query);

    // Append the signature (not URL-encoded again — it's already in canonical form).
    canonical_query += '&';
    canonical_query += "Signature=";
    canonical_query += yaddnsc::sdk::url_encode(signature);

    // Assemble the request.
    HttpRequest request{};
    request.url = std::string(API_URL);
    request.body = std::move(canonical_query);
    request.content_type = "application/x-www-form-urlencoded";
    request.method = Method::POST;

    return request;
}

bool AlibabaCloudDriver::check_response(const HttpResponse& response, const Services& services) {
    YADDNSC_SDK_LOG_TRACE(services, "Got {} from server.", response.body);

    if (response.status_code == 200) {
        // On success, Alibaba DNS returns JSON with RecordId.
        if (auto result = parse_response<AlibabaUpdateResponse>(response.body)) {
            YADDNSC_SDK_LOG_DEBUG(services, "DNS record updated successfully (RecordId: {})", result->record_id);
            return true;
        }

        // If we can't parse the expected success response, try error format.
        YADDNSC_SDK_LOG_ERROR(services, "Failed to parse Alibaba Cloud API response");
        return false;
    }

    // Error responses include JSON with Code and Message.
    if (!response.body.empty()) {
        if (auto result = parse_response<AlibabaErrorResponse>(response.body)) {
            YADDNSC_SDK_LOG_ERROR(services, "Alibaba Cloud API error: {} ({})", result->message, result->code);
        } else {
            YADDNSC_SDK_LOG_ERROR(services, "Alibaba Cloud API error (HTTP {}): {}", response.status_code,
                                  response.body);
        }
    } else {
        YADDNSC_SDK_LOG_ERROR(services, "Alibaba Cloud API request failed with HTTP status {}", response.status_code);
    }

    return false;
}
