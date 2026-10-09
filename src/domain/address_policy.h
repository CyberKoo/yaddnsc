#ifndef YADDNSC_DOMAIN_ADDRESS_POLICY_H
#define YADDNSC_DOMAIN_ADDRESS_POLICY_H

#include <optional>
#include <vector>

#include "domain/dns/record_kind.h"
#include "domain/network/inet_address.h"

namespace domain {

/// Address selection policy for a subdomain update.
///
/// Pure domain rules; not a general "prefer global unicast" ranking.
struct AddressPolicy {
    bool allow_ula{false};         ///< Allow ULA (fc00::/7) for AAAA candidates
    bool allow_local_link{false};  ///< Allow link-local (fe80::/10) for AAAA candidates
};

/// Apply the address policy and pick the address to publish.
///
/// Rules:
///  - A and AAAA candidates must match the record's address family;
///  - ULA and link-local filtering applies only to AAAA;
///  - an empty candidate list (before or after filtering) means "no address";
///  - the first surviving candidate wins.
[[nodiscard]] std::optional<InetAddress> select_address(std::vector<InetAddress> candidates, RecordKind record_type,
                                                        const AddressPolicy& policy);

}  // namespace domain

#endif  // YADDNSC_DOMAIN_ADDRESS_POLICY_H
