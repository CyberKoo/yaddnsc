#ifndef YADDNSC_SUPPORT_REDACT_HPP
#define YADDNSC_SUPPORT_REDACT_HPP

/// Host-side redaction for log output and diagnostics: address fields
/// (resolver servers, ip_source_param) are plain strings that may embed
/// credentials. The driver-plugin counterpart lives in
/// <yaddnsc/sdk/redact.hpp>.

#include <string>

namespace Utils {

/// Replace URI userinfo ("scheme://user:pass@host/...") with "***".
inline void redact_uri_credentials(std::string& text) {
    const auto scheme_end = text.find("://");
    if (scheme_end == std::string::npos) {
        return;
    }
    const auto authority_start = scheme_end + 3;
    const auto authority_end = text.find('/', authority_start);
    const auto at = text.find('@', authority_start);
    if (at != std::string::npos && (authority_end == std::string::npos || at < authority_end)) {
        text.replace(authority_start, at - authority_start, "***");
    }
}

/// The redacted copy, for log sites that format an address inline.
[[nodiscard]] inline std::string redacted_uri_credentials(std::string text) {
    redact_uri_credentials(text);
    return text;
}

}  // namespace Utils

#endif  // YADDNSC_SUPPORT_REDACT_HPP
