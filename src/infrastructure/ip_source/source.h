//
// ip_source — shared IP-source result type.
//

#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_SOURCE_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_SOURCE_H

#include <vector>

#include <expected>

#include "domain/error/error.h"
#include "domain/network/inet_address.h"

namespace ipsource {

/// Result of one source lookup: candidate addresses, or a structured failure.
using Result = std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>;

}  // namespace ipsource

#endif  // YADDNSC_INFRASTRUCTURE_IP_SOURCE_SOURCE_H
