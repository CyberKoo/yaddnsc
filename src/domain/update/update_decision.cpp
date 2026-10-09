#include "update_decision.h"

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
    if (!current_records.empty() && current_records.front() == *local_address) {
        return UpdateDecision::SKIP_UNCHANGED;
    }
    return UpdateDecision::UPDATE_CHANGED;
}

}  // namespace domain
