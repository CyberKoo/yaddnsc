//
// Percent-encoding for the net::http client domain.
//
// application/x-www-form-urlencoded rules: unreserved characters pass
// through, spaces become '+', everything else is %XX (uppercase hex).
//

#ifndef YADDNSC_HTTP_CLIENT_FORM_ENCODE_H
#define YADDNSC_HTTP_CLIENT_FORM_ENCODE_H

#include <map>
#include <string>
#include <string_view>

namespace net::http {

/// Encode a single value (or name) per form semantics.
[[nodiscard]] std::string encode_form_component(std::string_view value);

/// Encode a parameter map as "k1=v1&k2=v2" (keys and values encoded,
/// pairs joined by '&').
[[nodiscard]] std::string encode_form(const std::multimap<std::string, std::string>& params);

}  // namespace net::http

#endif  // YADDNSC_HTTP_CLIENT_FORM_ENCODE_H
