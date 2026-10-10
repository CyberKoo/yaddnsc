//
// /etc/resolv.conf nameserver discovery.
//

#include "resolv_conf.h"

#include <optional>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <string>

#include "domain/network/inet_address.h"

namespace dns {

std::vector<domain::DnsServer> parse_resolv_conf(const std::filesystem::path& path) {
    std::vector<domain::DnsServer> servers;

    std::ifstream in(path);
    if (!in) {
        SPDLOG_DEBUG(R"(Cannot open "{}" — no resolv.conf bootstrap servers)", path.string());
        return servers;
    }

    std::string line;
    while (std::getline(in, line)) {
        // Strip comments: both '#' and ';' start a comment in resolv.conf.
        if (const auto pos = line.find_first_of("#;"); pos != std::string::npos) {
            line.erase(pos);
        }

        std::istringstream tokens(line);
        std::string directive;
        std::string value;
        if (!(tokens >> directive >> value) || directive != "nameserver") {
            continue;
        }

        if (domain::InetAddress::parse(value)) {
            servers.push_back(domain::DnsServer{.address = value, .port = 53});
        } else {
            SPDLOG_DEBUG(R"(Ignoring invalid nameserver entry "{}" in "{}")", value, path.string());
        }
    }

    return servers;
}

}  // namespace dns
