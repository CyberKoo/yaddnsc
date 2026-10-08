//
// Domain — everything one driver update call needs, in domain terms.
//

#ifndef YADDNSC_DOMAIN_UPDATE_DRIVER_UPDATE_COMMAND_H
#define YADDNSC_DOMAIN_UPDATE_DRIVER_UPDATE_COMMAND_H

#include <string>

namespace domain {

/// DriverUpdateCommand — the input of one driver update.
///
/// A pure value carried from the update workflow to the driver gateway; it
/// names no plugin-SDK type, so the domain can own it (DriverError is already
/// here).
struct DriverUpdateCommand {
    std::string driver_params;  ///< Opaque driver configuration JSON text
    std::string ip_addr;        ///< Resolved IP address to publish
    std::string rd_type;        ///< DNS record type as string (e.g. "A", "AAAA")
    std::string domain;         ///< Parent domain name
    std::string subdomain;      ///< Subdomain label (may be "@" for apex)
    std::string fqdn;           ///< Fully qualified domain name
};

}  // namespace domain

#endif  // YADDNSC_DOMAIN_UPDATE_DRIVER_UPDATE_COMMAND_H
