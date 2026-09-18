//
// Created by Kotarou on 2026/9/17.
//

#include "static_validator.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <yaddnsc/util/format.hpp>

#include "domain/config/dns_config.h"
#include "domain/config/ip_source_kind.h"
#include "domain/dns/record_kind.h"
#include "domain/fqdn.h"
#include "domain/network/inet_address.h"
#include "infrastructure/config/config.h"
#include "infrastructure/network/uri.h"
#include "support/fmt.hpp"
#include "support/util/validation.hpp"

#include "min_update_interval.h"
#include "normalizer.h"

namespace Config {
namespace {
using Code = domain::ConfigError::Code;

void push_error(std::vector<domain::ConfigError>& errors, Code code, std::string message) {
    errors.push_back(domain::ConfigError{.code = code, .message = std::move(message)});
}

/// Static IP source checks for a subdomain. Messages are verbatim copies
/// of the legacy ConfigValidator ones.
void validate_ip_source(std::vector<domain::ConfigError>& errors,
                        const std::string& domain_name,
                        const SubdomainConfig& subdomain) {
    const auto fqdn = domain::make_fqdn(domain_name, subdomain.name);

    // Only the INTERFACE source strictly requires a network interface name.
    if (subdomain.ip_source == IpSource::INTERFACE && subdomain.interface.empty()) {
        push_error(errors, Code::MISSING_INTERFACE,
                   fmt::format("Subdomain {} uses interface IP source but 'interface' field is empty", fqdn));
    }

    if (subdomain.ip_source == IpSource::HTTP) {
        if (subdomain.ip_source_param.empty()) {
            push_error(errors, Code::EMPTY_IP_SOURCE_PARAM,
                       fmt::format("Subdomain {} uses HTTP IP source but ip_source_param is empty", fqdn));
        } else {
            try {
                const auto uri = Uri::parse(subdomain.ip_source_param);
                if (uri.get_host().empty() || uri.get_port() == 0) {
                    throw std::runtime_error("missing host or port");
                }
            } catch (const std::exception& e) {
                push_error(errors, Code::INVALID_IP_SOURCE_URL,
                           fmt::format("Subdomain {} has invalid ip_source_param '{}': {}", fqdn,
                                       subdomain.ip_source_param, e.what()));
            }
        }
        return;
    }

    if (subdomain.ip_source == IpSource::MDNS) {
        if (subdomain.ip_source_param.empty()) {
            push_error(errors, Code::EMPTY_IP_SOURCE_PARAM,
                       fmt::format("Subdomain {} uses mDNS IP source but ip_source_param is empty", fqdn));
            return;
        }

        if (!Utils::is_valid_domain(subdomain.ip_source_param)) {
            push_error(errors, Code::INVALID_MDNS_NAME,
                       fmt::format("Subdomain {} has invalid domain name '{}' for mDNS IP source", fqdn,
                                   subdomain.ip_source_param));
        }

        // mDNS uses the .local TLD (RFC 6762 §3).
        const auto& param = subdomain.ip_source_param;
        if (!param.ends_with(".local") && !param.ends_with(".local.")) {
            push_error(
                errors, Code::MDNS_NOT_LOCAL,
                fmt::format("Subdomain {} uses mDNS IP source but domain '{}' does not end with '.local' (RFC 6762)",
                            fqdn, subdomain.ip_source_param));
        }

        // A missing type defaults to A at runtime, so it is mDNS-compatible.
        const auto record_kind = subdomain.type.value_or(RecordKind::A);
        if (record_kind != RecordKind::A && record_kind != RecordKind::AAAA) {
            push_error(errors, Code::MDNS_BAD_RECORD_TYPE,
                       fmt::format("Subdomain {} uses mDNS IP source but type must be 'a' or 'aaaa'", fqdn));
        }
    }
}

/// Static resolver address check. Uri::parse exceptions deliberately
/// escape (same behaviour as the legacy validator).
void validate_resolver_address(std::vector<domain::ConfigError>& errors, const std::string& address) {
    const auto uri = Uri::parse(address);
    // DoH / DoT address — starts with https or tls.
    if (uri.get_schema() == "https" || uri.get_schema() == "tls") {
        if (uri.get_host().empty()) {
            push_error(errors, Code::INVALID_RESOLVER,
                       fmt::format(R"(DoH/DoT resolver address "{}" has an empty host)", address));
        }
        if (uri.get_port() == 0) {
            push_error(errors, Code::INVALID_RESOLVER,
                       fmt::format(R"(DoH/DoT resolver address "{}" has port 0)", address));
        }
        return;
    }

    // Plain DNS address — must be a valid IP.
    if (!InetAddress::parse(address)) {
        push_error(errors, Code::INVALID_RESOLVER, fmt::format("Invalid resolver address {}", address));
    }
}
}  // namespace

auto validate_static(const AppConfig& raw) -> std::vector<domain::ConfigError> {
    std::vector<domain::ConfigError> errors;

    for (const auto& [name, update_interval, force_update, driver, subdomains] : raw.domains) {
        if (name.empty()) {
            push_error(errors, Code::EMPTY_DOMAIN_NAME, "Domain name must not be empty");
        }

        if (subdomains.empty()) {
            push_error(errors, Code::EMPTY_SUBDOMAINS,
                       fmt::format("Domain '{}' must have at least one subdomain", name));
        }

        if (update_interval < YADDNSC_MIN_UPDATE_INTERVAL) {
            push_error(errors, Code::UPDATE_INTERVAL_LOW,
                       fmt::format("Update interval too low for domain {} ({}), minimal interval: {}", name,
                                   update_interval, YADDNSC_MIN_UPDATE_INTERVAL));
        }

        if (force_update != 0 && force_update < update_interval) {
            push_error(errors, Code::FORCE_UPDATE_CONFLICT,
                       fmt::format("Force update interval for domain {} must not be smaller than the update interval "
                                   "({})",
                                   name, update_interval));
        }

        for (const auto& subdomain : subdomains) {
            if (subdomain.name.empty()) {
                push_error(errors, Code::EMPTY_SUBDOMAIN_NAME,
                           fmt::format("Subdomain name must not be empty in domain '{}'", name));
            }

            validate_ip_source(errors, name, subdomain);

            if (subdomain.update_interval != 0 && subdomain.update_interval < YADDNSC_MIN_UPDATE_INTERVAL) {
                push_error(errors, Code::UPDATE_INTERVAL_LOW,
                           fmt::format("Update interval too low for subdomain {}.{} ({}), minimal interval: {}",
                                       subdomain.name, name, subdomain.update_interval, YADDNSC_MIN_UPDATE_INTERVAL));
            }
        }
    }

    // Custom resolver address(es) — Uri::parse exceptions escape on purpose.
    if (raw.resolver.use_custom_server) {
        if (raw.resolver.servers.empty() && raw.resolver.address.empty()) {
            push_error(errors, Code::NO_RESOLVER_SERVERS,
                       "use_custom_server is enabled but no custom resolver servers are configured");
        }
        if (!raw.resolver.servers.empty()) {
            for (const auto& server : raw.resolver.servers) {
                validate_resolver_address(errors, server.address);
            }
        } else if (!raw.resolver.address.empty()) {
            validate_resolver_address(errors, raw.resolver.address);
        }
    }

    return errors;
}

auto validate_and_normalize(const AppConfig& raw)
    -> std::expected<domain::RuntimeConfig, std::vector<domain::ConfigError>> {
    auto errors = validate_static(raw);
    if (!errors.empty()) {
        return std::unexpected(std::move(errors));
    }
    return normalize(raw);
}

}  // namespace Config
