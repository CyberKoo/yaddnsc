#include "fqdn.h"

#include <string>
#include <string_view>

namespace domain {

std::string make_fqdn(std::string_view domain, std::string_view subdomain) {
    if (subdomain.empty() || subdomain == "@") {
        return std::string(domain);
    }
    return std::string(subdomain) + "." + std::string(domain);
}

}  // namespace domain
