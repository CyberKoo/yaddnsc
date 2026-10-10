#include "static_validator.h"

#include <yaddnsc/util/format.hpp>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <optional>

#include "domain/config/dns_config.h"
#include "domain/config/ip_source_kind.h"
#include "domain/dns/record_kind.h"
#include "domain/fqdn.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/ip_literal.h"
#include "infrastructure/uri/uri.h"
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
void validate_ip_source(std::vector<domain::ConfigError>& errors, const std::string& domain_name,
                        const SubdomainConfig& subdomain) {
    const auto fqdn = domain::make_fqdn(domain_name, subdomain.name);
    // An absent ip_source key normalises to INTERFACE; validate the
    // effective value.
    const auto ip_source = subdomain.ip_source.value_or(domain::IpSource::INTERFACE);

    // Only the INTERFACE source strictly requires a network interface name.
    if (ip_source == domain::IpSource::INTERFACE && subdomain.interface.empty()) {
        push_error(errors, Code::MISSING_INTERFACE,
                   fmt::format("Subdomain {} uses interface IP source but 'interface' field is empty", fqdn));
    }

    if (ip_source == domain::IpSource::HTTP) {
        if (subdomain.ip_source_param.empty()) {
            push_error(errors, Code::EMPTY_IP_SOURCE_PARAM,
                       fmt::format("Subdomain {} uses HTTP IP source but ip_source_param is empty", fqdn));
        } else {
            const auto uri = Uri::parse(subdomain.ip_source_param);
            if (!uri.has_value()) {
                push_error(errors, Code::INVALID_IP_SOURCE_URL,
                           fmt::format("Subdomain {} has invalid ip_source_param '{}': {}", fqdn,
                                       subdomain.ip_source_param, error_message(uri.error())));
            } else if (uri->get_host().empty() || uri->get_port() == 0) {
                push_error(errors, Code::INVALID_IP_SOURCE_URL,
                           fmt::format("Subdomain {} has invalid ip_source_param '{}': missing host or port", fqdn,
                                       subdomain.ip_source_param));
            }
        }
        return;
    }

    if (ip_source == domain::IpSource::MDNS) {
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
        const auto record_kind = subdomain.type.value_or(domain::RecordKind::A);
        if (record_kind != domain::RecordKind::A && record_kind != domain::RecordKind::AAAA) {
            push_error(errors, Code::MDNS_BAD_RECORD_TYPE,
                       fmt::format("Subdomain {} uses mDNS IP source but type must be 'a' or 'aaaa'", fqdn));
        }
    }
}

/// Static resolver address check. Uri::parse failures are reported as
/// INVALID_RESOLVER config errors — a malformed address is a configuration
/// mistake, so it must surface identically on every entry path (run, config
/// test, dns resolve) instead of escaping as an "unhandled exception".
void validate_resolver_address(std::vector<domain::ConfigError>& errors, const std::string& address) {
    const auto uri = Uri::parse(address);
    if (!uri.has_value()) {
        push_error(errors, Code::INVALID_RESOLVER,
                   fmt::format(R"(Malformed resolver address "{}": {})", address, error_message(uri.error())));
        return;
    }
    // DoH / DoT address — starts with https or tls.
    if (uri->get_schema() == "https" || uri->get_schema() == "tls") {
        if (uri->get_host().empty()) {
            push_error(errors, Code::INVALID_RESOLVER,
                       fmt::format(R"(DoH/DoT resolver address "{}" has an empty host)", address));
        }
        if (uri->get_port() == 0) {
            push_error(errors, Code::INVALID_RESOLVER,
                       fmt::format(R"(DoH/DoT resolver address "{}" has port 0)", address));
        }
        return;
    }

    // A scheme other than https (DoH) or tls (DoT) has no resolver backend:
    // the factory throws std::invalid_argument for it, which would escape as
    // an unhandled exception on the run path. Report it as a config error.
    if (!uri->get_schema().empty()) {
        push_error(errors, Code::INVALID_RESOLVER,
                   fmt::format(R"(Resolver address "{}" has unsupported scheme "{}"; )"
                               R"(use "https", "tls" or a bare IP literal)",
                               address, uri->get_schema()));
        return;
    }

    // A classic resolver dials the host and DnsServer::port only, so a path
    // or query written into the address would be silently dropped.
    if (!uri->get_path().empty() || !uri->get_query_string().empty()) {
        push_error(errors, Code::INVALID_RESOLVER,
                   fmt::format(R"(Resolver address "{}" carries a path or query; a classic resolver )"
                               R"(dials host:port only)",
                               address));
        return;
    }

    // Plain DNS address — must be a valid IP literal. bare_ip_host judges
    // the authority host in either spelling (bare or bracketed IPv6); the
    // brackets are URI syntax, not part of the address.
    if (!bare_ip_host(*uri)) {
        push_error(errors, Code::INVALID_RESOLVER, fmt::format("Invalid resolver address {}", address));
        return;
    }

    // A classic resolver's endpoint comes from DnsServer::port, never from
    // the authority, so a port here would be silently dropped and the query
    // would go to a different endpoint than configured. Reject it and point
    // at the field that is actually honoured.
    if (uri->get_port() != 0) {
        push_error(
            errors, Code::INVALID_RESOLVER,
            fmt::format(R"(Resolver address "{}" carries a port; set "port" on the server entry instead)", address));
    }
}
}  // namespace

auto validate_static(const AppConfig& raw) -> std::vector<domain::ConfigError> {
    std::vector<domain::ConfigError> errors;

    if (raw.domains.empty()) {
        push_error(errors, Code::EMPTY_DOMAINS, "Config must define at least one domain");
    }

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

        if (force_update < 0) {
            push_error(errors, Code::FORCE_UPDATE_CONFLICT,
                       fmt::format("Field 'force_update' for domain {} must not be negative (got {}, 0 disables it)",
                                   name, force_update));
        } else if (force_update != 0 && force_update < update_interval) {
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

    // Custom resolver address(es) — parse failures are collected here as
    // INVALID_RESOLVER config errors (see validate_resolver_address).
    if (raw.resolver.use_custom_servers) {
        if (raw.resolver.servers.empty()) {
            push_error(errors, Code::NO_RESOLVER_SERVERS,
                       "use_custom_servers is enabled but no custom resolver servers are configured");
        }
        for (const auto& server : raw.resolver.servers) {
            validate_resolver_address(errors, server.address);
        }
    }

    // Bootstrap DNS server — must be an IP literal when set (hostnames would
    // be circular: bootstrap DNS is what resolves hostnames). Both IPv6
    // spellings are accepted, but there is no field to carry a scheme, port
    // or path, so they are rejected here rather than dropped later.
    if (!raw.bootstrap_dns.empty()) {
        const auto uri = Uri::parse(raw.bootstrap_dns);
        const bool literal = uri.has_value() && uri->get_schema().empty() && uri->get_port() == 0 &&
                             uri->get_path().empty() && uri->get_query_string().empty() &&
                             bare_ip_host(*uri).has_value();
        if (!literal) {
            push_error(errors, Code::INVALID_BOOTSTRAP_DNS,
                       fmt::format(R"(Invalid bootstrap_dns "{}": must be an IP literal, with or without )"
                                   R"(brackets (e.g. "223.5.5.5", "2606:4700:4700::1111", )"
                                   R"("[2606:4700:4700::1111]"))",
                                   raw.bootstrap_dns));
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
