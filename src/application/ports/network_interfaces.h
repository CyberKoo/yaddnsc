//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_PORTS_NETWORK_INTERFACES_H
#define YADDNSC_APPLICATION_PORTS_NETWORK_INTERFACES_H

#include <string>
#include <vector>

#include "domain/network/inet_address.h"

/// NetworkInterfaces — application port for querying the host's network
/// interfaces (names and assigned addresses).
///
/// Used by the environment validator (does a configured interface exist?)
/// and by the `interface list` / `interface ip` diagnostic commands; neither
/// is allowed to call getifaddrs() directly from the application layer.
///
/// Error contract: addresses() throws std::runtime_error when the interface
/// does not exist — the legacy InterfaceUtil wording is preserved verbatim
/// for the CLI's "Error: ..." output. names() never throws.
class NetworkInterfaces {
public:
    virtual ~NetworkInterfaces() = default;

    /// Names of all interfaces that carry at least one IPv4/IPv6 address.
    [[nodiscard]] virtual std::vector<std::string> names() const = 0;

    /// All addresses (v4 and v6) assigned to `name`.
    /// @throws std::runtime_error  If the interface does not exist.
    [[nodiscard]] virtual std::vector<InetAddress> addresses(const std::string &name) const = 0;
};

#endif // YADDNSC_APPLICATION_PORTS_NETWORK_INTERFACES_H
