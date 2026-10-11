#include "update_decision.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace domain {

UpdateDecision decide_update(const std::vector<std::string>& current_records,
                             const std::optional<std::string>& local_address, bool force_update) noexcept {
    if (!local_address.has_value()) {
        return UpdateDecision::SKIP_NO_ADDRESS;
    }
    if (force_update) {
        return UpdateDecision::UPDATE_FORCED;
    }
    // DNS cannot identify the provider record_id: a matching sibling record
    // must not hide a stale managed record. Only an entirely matching RRset
    // provides enough evidence to skip the update.
    if (!current_records.empty() && std::ranges::all_of(current_records, [&local_address](const std::string& record) {
            return record == *local_address;
        })) {
        return UpdateDecision::SKIP_UNCHANGED;
    }
    return UpdateDecision::UPDATE_CHANGED;
}

}  // namespace domain
