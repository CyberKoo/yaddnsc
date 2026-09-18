//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DOMAIN_UPDATE_UPDATE_DECISION_H
#define YADDNSC_DOMAIN_UPDATE_UPDATE_DECISION_H

#include <optional>
#include <string>
#include <vector>

/// UpdateDecision — pure decision over already-fetched inputs for one update
/// cycle.
///
/// Locked behaviours (legacy Updater semantics):
///   - no local address          → SKIP_NO_ADDRESS (the driver is never called);
///   - force_update              → UPDATE_FORCED (DNS records are not consulted);
///   - the FIRST record equals the local address → SKIP_UNCHANGED;
///   - everything else — including an empty record list, whether the lookup
///     succeeded empty or failed and was mapped to empty — is UPDATE_CHANGED.
/// Only the first record is ever compared; no "skip because no record" state
/// exists.
namespace domain {

enum class UpdateDecision {
    SKIP_UNCHANGED,  ///< First DNS record already equals the local address
    UPDATE_CHANGED,  ///< Records differ (or are unavailable) — update
    UPDATE_FORCED,   ///< force_update cycle — update without comparing
    SKIP_NO_ADDRESS, ///< No usable local address — do not call the driver
};

/// Decide what one update cycle should do.
///
/// @param current_records  DNS records to compare against; a failed lookup
///                         arrives as an empty list (mapped by the workflow).
/// @param local_address    The address to publish, if any was resolved.
/// @param force_update     Skip the comparison and update unconditionally.
[[nodiscard]] inline UpdateDecision decide_update(const std::vector<std::string> &current_records,
                                                  const std::optional<std::string> &local_address,
                                                  bool force_update) noexcept {
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

} // namespace domain

#endif // YADDNSC_DOMAIN_UPDATE_UPDATE_DECISION_H
