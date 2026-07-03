//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_UTIL_VALIDATION_H
#define YADDNSC_UTIL_VALIDATION_H

#include <regex>
#include <string_view>

namespace Utils {
    static constexpr int DOMAIN_NAME_MAX_LEN = 253;

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
