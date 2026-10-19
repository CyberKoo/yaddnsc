#ifndef YADDNSC_DRV_ROUTE53_XML_PAYLOAD_H
#define YADDNSC_DRV_ROUTE53_XML_PAYLOAD_H

/// XML payloads of the Route 53 ChangeResourceRecordSets call.
///
/// One module owns both directions: the UPSERT change batch the driver sends,
/// and the response bodies it reads back (a success document carrying
/// <ChangeInfo><Status>, or an <ErrorResponse> carrying one or more
/// <Error><Code>/<Message> entries).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace route53 {

/// The Route 53 XML namespace URI (char array: passed to libxml2 C APIs that
/// require null-terminated strings).
inline constexpr char R53_XMLNS[] = "https://route53.amazonaws.com/doc/2013-04-01/";

/// What could be read from a ChangeResourceRecordSets response body.
struct ChangeResponse {
    bool parsed{};                      ///< false when libxml2 rejected the body
    bool namespace_registered{};        ///< false when the Route 53 namespace could not be registered
    std::optional<std::string> status{};  ///< <ChangeInfo><Status> text, empty when the element is absent
};

/// One <Error> entry of an error response, in document order.
struct ApiError {
    std::optional<std::string> code;     ///< <Code> text, empty when the element is absent
    std::optional<std::string> message;  ///< <Message> text, empty when the element is absent
};

/// Build the ChangeResourceRecordSets UPSERT body for one record.
///
/// @p fqdn must be a null-terminated record name (Route 53 wants the trailing
/// dot); returns an empty string when libxml2 cannot assemble the document.
[[nodiscard]] std::string build_change_body(const std::string& fqdn, std::string_view rd_type, std::string_view ip_addr,
                                            int ttl);

/// Read the <ChangeInfo><Status> outcome of a success response body.
[[nodiscard]] ChangeResponse parse_change_response(std::string_view body);

/// Read the <Error> entries of an error response body.
///
/// Returns an empty list when the body is not usable XML or carries no <Error>
/// entry, so the caller can fall back to reporting the HTTP status alone.
[[nodiscard]] std::vector<ApiError> parse_error_response(std::string_view body);

}  // namespace route53

#endif  // YADDNSC_DRV_ROUTE53_XML_PAYLOAD_H
