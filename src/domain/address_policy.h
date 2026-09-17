//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DOMAIN_ADDRESS_POLICY_H
#define YADDNSC_DOMAIN_ADDRESS_POLICY_H

#include <optional>
#include <vector>

#include "network/inet_address.h"
#include "record_kind.h"

namespace domain {

/// Address selection policy for a subdomain update.
///
/// Pure domain rules, moved verbatim from Updater (Phase 2). Deliberately NOT
/// a general "prefer global unicast" ranking: only the legacy filtering rules
/// exist here.
struct AddressPolicy {
    bool allow_ula{false};         ///< Allow ULA (fc00::/7) for AAAA candidates
    bool allow_local_link{false};  ///< Allow link-local (fe80::/10) for AAAA candidates
};

/// Apply the address policy and pick the address to publish.
///
/// Rules (identical to the legacy Updater behaviour):
///  - filtering applies only when the record type is AAAA;
///  - an empty candidate list (before or after filtering) means "no address";
///  - the first surviving candidate wins.
[[nodiscard]] inline std::optional<InetAddress>
select_address(std::vector<InetAddress> candidates, RecordKind record_type, const AddressPolicy &policy) {
    if (record_type == RecordKind::AAAA) {
        if (!policy.allow_local_link) {
            std::erase_if(candidates, [](const InetAddress &a) { return a.is_link_local(); });
        }
        if (!policy.allow_ula) {
            std::erase_if(candidates, [](const InetAddress &a) { return a.is_ula(); });
        }
    }

    if (candidates.empty()) {
        return std::nullopt;
    }
    return candidates.front();
}

} // namespace domain

#endif // YADDNSC_DOMAIN_ADDRESS_POLICY_H
