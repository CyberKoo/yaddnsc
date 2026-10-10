#include "ip_literal.h"

#include <optional>
#include <string_view>

#include "domain/network/inet_address.h"
#include "infrastructure/uri/uri.h"

namespace Config {

std::optional<std::string_view> bare_ip_host(const Uri& uri) {
    // get_host() already strips the brackets of an IP-literal authority, so
    // the same inet_pton call covers "2606:4700:4700::1111" and
    // "[2606:4700:4700::1111]" without a second spelling rule here.
    const std::string_view host = uri.get_host();
    if (host.empty() || !domain::InetAddress::parse(host)) {
        return std::nullopt;
    }
    return host;
}

}  // namespace Config
