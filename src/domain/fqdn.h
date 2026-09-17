//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DOMAIN_FQDN_H
#define YADDNSC_DOMAIN_FQDN_H

#include <string>
#include <string_view>

#include "fmt.hpp"

namespace domain {
    /// Build the FQDN for a subdomain within a domain.
    ///
    /// Apex labels (`"@"` or empty) resolve to the bare domain so that DNS
    /// lookups and driver `{fqdn}` substitutions target `example.com` rather
    /// than `@.example.com`.
    [[nodiscard]] inline std::string make_fqdn(std::string_view domain, std::string_view subdomain) {
        if (subdomain.empty() || subdomain == "@") {
            return std::string(domain);
        }
        return fmt::format("{}.{}", subdomain, domain);
    }
}

#endif // YADDNSC_DOMAIN_FQDN_H
