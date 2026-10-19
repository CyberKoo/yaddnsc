#include "bootstrap_dns.h"

#include <optional>
#include <string>

#include "infrastructure/config/ip_literal.h"
#include "infrastructure/uri/uri.h"

namespace Config {

BootstrapDns resolve_bootstrap_dns(std::string_view field) {
    if (field.empty()) {
        return {.address = {}, .valid = true};
    }

    const auto uri = Uri::parse(field);
    if (!uri) {
        return {.address = std::string{field}, .valid = false};
    }

    const auto host = bare_ip_host(*uri);
    if (!host) {
        return {.address = std::string{field}, .valid = false};
    }

    return {.address = std::string{*host},
            .valid = uri->get_schema().empty() && uri->get_port() == 0 && uri->get_path().empty() &&
                     uri->get_query_string().empty()};
}

}  // namespace Config
