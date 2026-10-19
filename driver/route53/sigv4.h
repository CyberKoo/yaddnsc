#ifndef YADDNSC_DRV_ROUTE53_SIGV4_H
#define YADDNSC_DRV_ROUTE53_SIGV4_H

/// AWS SigV4 request signing for the Route 53 ChangeResourceRecordSets call.
///
/// The driver builds the XML payload and the request URL; this module owns the
/// canonical request, the credential scope, the signing key derivation and the
/// resulting header values. Every byte of the canonical form is fixed by AWS:
/// callers pass the same path string they put in the request URL and the exact
/// payload they send.

#include <string>
#include <string_view>

namespace route53 {

/// Route 53 API hostname. Signed as the `host` header, so the request URL and
/// the canonical headers must use this same value.
inline constexpr std::string_view R53_HOST = "route53.amazonaws.com";

/// The header values a SigV4 signature covers for one request.
struct SignedHeaders {
    std::string amz_date;        ///< X-Amz-Date value ("YYYYMMDDTHHmmSSZ")
    std::string content_sha256;  ///< X-Amz-Content-SHA256 value (the payload hash)
    std::string authorization;   ///< Authorization header value
};

/// Sign a POST of @p body at @p uri_path and return the SigV4 headers.
///
/// @p uri_path must be the path used verbatim in the request URL (it becomes
/// the canonical URI); @p body must be the payload sent on the wire, because
/// its SHA-256 is both signed and transmitted as X-Amz-Content-SHA256.
[[nodiscard]] SignedHeaders sign_request(std::string_view access_key_id, std::string_view secret_access_key,
                                         std::string_view region, std::string_view uri_path, std::string_view body);

}  // namespace route53

#endif  // YADDNSC_DRV_ROUTE53_SIGV4_H
