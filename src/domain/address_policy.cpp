//
// Created by Kotarou on 2026/9/27.
//

#include "address_policy.h"

#include <optional>
#include <vector>

#include "domain/network/address_family.h"

namespace domain {

std::optional<InetAddress> select_address(std::vector<InetAddress> candidates, RecordKind record_type,
                                          const AddressPolicy& policy) {
    if (record_type == RecordKind::A || record_type == RecordKind::AAAA) {
        const auto family = record_type == RecordKind::A ? AddressFamily::IPV4 : AddressFamily::IPV6;
        std::erase_if(candidates, [family](const InetAddress& addr) { return addr.get_family() != family; });
    }

    if (record_type == RecordKind::AAAA) {
        if (!policy.allow_local_link) {
            std::erase_if(candidates, [](const InetAddress& a) { return a.is_link_local(); });
        }
        if (!policy.allow_ula) {
            std::erase_if(candidates, [](const InetAddress& a) { return a.is_ula(); });
        }
    }

    if (candidates.empty()) {
        return std::nullopt;
    }
    return candidates.front();
}

}  // namespace domain
