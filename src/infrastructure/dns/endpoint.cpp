#include "endpoint.h"

#include <string_view>
#include <utility>

#include "domain/config/dns_config.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"

namespace dns {

std::expected<ResolverEndpoint, std::vector<std::string>> parse_resolver_endpoint(const domain::DnsServer& server) {
    const auto& address = server.address;
    auto uri = Uri::parse(address);
    std::vector<std::string> errors;
    if (!uri) {
        errors.push_back(fmt::format(R"(Malformed resolver address "{}": {})", address, error_message(uri.error())));
        return std::unexpected(std::move(errors));
    }
    const auto scheme = uri->get_schema();
    if (scheme == "https" || scheme == "tls") {
        if (uri->get_host().empty()) {
            errors.push_back(fmt::format(R"(DoH/DoT resolver address "{}" has an empty host)", address));
        }
        const auto scheme_end = address.find("://");
        const auto authority_start = scheme_end == std::string::npos ? 0 : scheme_end + 3;
        if (scheme == "tls" && address.find_first_of("/?#", authority_start) != std::string::npos) {
            errors.emplace_back("DoT endpoint must not contain a path, query or fragment");
        }
        if (uri->get_port() == 0) {
            errors.push_back(fmt::format(R"(DoH/DoT resolver address "{}" has port 0)", address));
        }
        if (!errors.empty()) {
            return std::unexpected(std::move(errors));
        }
        const auto kind = scheme == "tls" ? ResolverEndpoint::Kind::DOT : ResolverEndpoint::Kind::DOH;
        const auto port = static_cast<std::uint16_t>(uri->get_port());
        return ResolverEndpoint{kind, std::move(*uri), port, std::nullopt};
    }
    if (!scheme.empty()) {
        errors.push_back(fmt::format(R"(Resolver address "{}" has unsupported scheme "{}"; )"
                                     R"(use "https", "tls" or a bare IP literal)",
                                     address, scheme));
    } else if (!uri->get_path().empty() || uri->has_query() || address.find('#') != std::string::npos) {
        errors.push_back(fmt::format(R"(Resolver address "{}" carries a path or query; a classic resolver )"
                                     R"(dials host:port only)",
                                     address));
    } else if (auto literal = domain::InetAddress::parse(uri->get_host()); !literal) {
        errors.push_back(fmt::format("Invalid resolver address {}", address));
    } else if (uri->get_port() != 0) {
        errors.push_back(
            fmt::format(R"(Resolver address "{}" carries a port; set "port" on the server entry instead)", address));
    } else if (server.port == 0) {
        errors.emplace_back("Classic DNS port must not be zero");
    } else {
        return ResolverEndpoint{ResolverEndpoint::Kind::CLASSIC, std::move(*uri), server.port, literal};
    }
    return std::unexpected(std::move(errors));
}

}  // namespace dns
