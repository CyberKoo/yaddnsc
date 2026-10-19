#include "redaction.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <glaze/json/generic_fwd.hpp>
#include <yaddnsc/util/format.hpp>

#include "domain/config/dns_config.h"
#include "infrastructure/config/config.h"
#include "support/fmt.hpp"
#include "support/redact.hpp"

#include "glaze_meta.hpp"  // IWYU pragma: keep

namespace {
using Utils::redact_uri_credentials;

/// Key-name matching is a substring rule on purpose: driver parameter names are
/// provider-defined, so the set of secret-bearing keys is not enumerable here.
[[nodiscard]] bool is_sensitive_key(std::string_view key) {
    std::string lower(key.size(), '\0');
    std::ranges::transform(key, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("token") != std::string::npos || lower.find("password") != std::string::npos ||
           lower.find("secret") != std::string::npos || lower.find("key") != std::string::npos;
}

void redact_sensitive_fields(glz::generic& value) {
    if (value.is_object()) {
        for (auto& entry : value.get_object()) {
            if (is_sensitive_key(entry.first)) {
                entry.second = "***";
            } else {
                redact_sensitive_fields(entry.second);
            }
        }
    } else if (value.is_array()) {
        for (auto& element : value.get_array()) {
            redact_sensitive_fields(element);
        }
    }
}
}  // namespace

std::string Config::redacted_json(AppConfig config) {
    for (auto& server : config.resolver.servers) {
        redact_uri_credentials(server.address);
    }
    for (auto& domain_config : config.domains) {
        for (auto& subdomain : domain_config.subdomains) {
            redact_uri_credentials(subdomain.ip_source_param);
            redact_sensitive_fields(subdomain.driver_params);
        }
    }

    std::string json;
    if (const auto ec = glz::write_json(config, json)) {
        throw std::runtime_error(fmt::format("Failed to serialize config: {}", glz::format_error(ec)));
    }
    return json;
}
