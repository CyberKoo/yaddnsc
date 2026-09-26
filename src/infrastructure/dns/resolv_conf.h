//
// /etc/resolv.conf nameserver discovery — the default source of bootstrap
// DNS servers when the configuration does not set bootstrap_dns.
//

#ifndef YADDNSC_DNS_RESOLV_CONF_H
#define YADDNSC_DNS_RESOLV_CONF_H

#include <filesystem>
#include <vector>

#include "domain/config/dns_config.h"

namespace DNS {

/// Parse nameserver entries from a resolv.conf-style file.
///
/// Only `nameserver <ip>` lines are considered; comments (# and ;), blank
/// lines and every other directive are ignored. Entries that are not valid
/// IP literals are skipped.
///
/// @param path  File to parse (defaults to /etc/resolv.conf).
/// @return      Discovered servers on port 53; empty when the file is
///              missing, unreadable or contains no valid entry (not an
///              error — the caller decides whether bootstrap is required).
[[nodiscard]] std::vector<Config::DnsServer> parse_resolv_conf(
    const std::filesystem::path& path = "/etc/resolv.conf");

}  // namespace DNS

#endif  // YADDNSC_DNS_RESOLV_CONF_H
