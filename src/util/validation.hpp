//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_UTIL_VALIDATION_H
#define YADDNSC_UTIL_VALIDATION_H

#include <regex>
#include <string_view>

namespace Utils {
    /// Maximum length of a fully qualified domain name (RFC 1035).
    static constexpr int DOMAIN_NAME_MAX_LEN = 253;

    /// Check whether a string is a valid domain name.
    ///
    /// Validates against RFC 1035 rules using a regex pattern.
    /// An empty string or a string without dots is rejected.
    ///
    /// @param domain  The domain name string to validate.
    /// @return        true if the domain name is syntactically valid.
    inline bool is_valid_domain(std::string_view domain) {
        const static std::regex domain_regex(
            R"(^(((?!-))(xn--|_)?[a-z0-9-]{0,61}[a-z0-9]{1,1}\.)*(xn--)?([a-z0-9][a-z0-9\-]{0,60}|[a-z0-9-]{1,30}\.[a-z]{2,})\.?$)");

        if (domain.length() > DOMAIN_NAME_MAX_LEN) {
            return false;
        }

        if (domain.find('.') != std::string_view::npos) {
            std::match_results<std::string_view::const_iterator> match;
            std::regex_match(domain.begin(), domain.end(), match, domain_regex);

            return !match.empty();
        }

        return false;
    }
}

#endif // YADDNSC_UTIL_VALIDATION_H
