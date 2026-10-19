#include "sigv4.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <yaddnsc/sdk/format.hpp>
#include <yaddnsc/sdk/signing.hpp>

namespace fmt = yaddnsc::sdk::fmt;

namespace route53 {
namespace {

/// SigV4 algorithm identifier.
constexpr std::string_view SIGV4_ALGORITHM = "AWS4-HMAC-SHA256";

/// Signed headers (sorted alphabetically).
constexpr std::string_view SIGNED_HEADERS = "host;x-amz-content-sha256;x-amz-date";

/// Service component of the credential scope.
constexpr std::string_view SERVICE = "route53";

/// Request-type component of the credential scope.
constexpr std::string_view REQUEST_TYPE = "aws4_request";

/// Derive the multi-stage SigV4 signing key.
[[nodiscard]] std::vector<std::uint8_t> derive_signing_key(std::string_view secret_access_key,
                                                           std::string_view date_stamp, std::string_view region) {
    const auto k_secret = fmt::format("AWS4{}", secret_access_key);

    const auto k_date = Signing::hmac_sha256(Signing::as_bytes(k_secret), Signing::as_bytes(date_stamp));
    const auto k_region = Signing::hmac_sha256(k_date, Signing::as_bytes(region));
    const auto k_service = Signing::hmac_sha256(k_region, Signing::as_bytes(SERVICE));
    return Signing::hmac_sha256(k_service, Signing::as_bytes(REQUEST_TYPE));
}

}  // anonymous namespace

SignedHeaders sign_request(std::string_view access_key_id, std::string_view secret_access_key, std::string_view region,
                           std::string_view uri_path, std::string_view body) {
    auto amz_date = Signing::iso8601_timestamp();     // "YYYYMMDDTHHmmSSZ"
    const auto date_stamp = Signing::iso8601_date();  // "YYYYMMDD"
    auto payload_hash = Signing::sha256_hex(body);

    // Canonical headers (sorted alphabetically), terminated by a newline.
    auto canonical_headers =
        fmt::format("host:{}\nx-amz-content-sha256:{}\nx-amz-date:{}\n", R53_HOST, payload_hash, amz_date);

    // Canonical request: method, URI, empty query, headers, signed set, hash.
    auto canonical_request =
        fmt::format("POST\n{}\n\n{}\n{}\n{}", uri_path, canonical_headers, SIGNED_HEADERS, payload_hash);
    auto canonical_request_hash = Signing::sha256_hex(canonical_request);

    // Credential scope and string-to-sign.
    auto credential_scope = fmt::format("{}/{}/{}/{}", date_stamp, region, SERVICE, REQUEST_TYPE);
    auto string_to_sign =
        fmt::format("{}\n{}\n{}\n{}", SIGV4_ALGORITHM, amz_date, credential_scope, canonical_request_hash);

    // Signature and Authorization header.
    auto signing_key = derive_signing_key(secret_access_key, date_stamp, region);
    auto signature_bytes = Signing::hmac_sha256(signing_key, Signing::as_bytes(string_to_sign));
    auto signature = Signing::hex_encode(signature_bytes);

    auto authorization = fmt::format("{} Credential={}/{}, SignedHeaders={}, Signature={}", SIGV4_ALGORITHM,
                                     access_key_id, credential_scope, SIGNED_HEADERS, signature);

    return {amz_date, payload_hash, authorization};
}

}  // namespace route53
