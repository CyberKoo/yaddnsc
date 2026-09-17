//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DOMAIN_UPDATE_UPDATE_DECISION_H
#define YADDNSC_DOMAIN_UPDATE_UPDATE_DECISION_H

#include <optional>
#include <string>
#include <vector>

/// UpdateDecision — pure decision over already-fetched inputs for one update
/// cycle (refactor/phase-3-scheduling-and-workflow.md §3.3).
///
/// Locked behaviours (Phase 0 table):
///   - no local address          → SkipNoAddress (the driver is never called);
///   - force_update              → UpdateForced (DNS records are not consulted);
///   - the FIRST record equals the local address → SkipUnchanged;
///   - everything else — including an empty record list, whether the lookup
///     succeeded empty or failed and was mapped to empty — is UpdateChanged.
/// Only the first record is ever compared; no "skip because no record" state
/// exists.
namespace domain {

enum class UpdateDecision {
    SkipUnchanged,  ///< First DNS record already equals the local address
    UpdateChanged,  ///< Records differ (or are unavailable) — update
    UpdateForced,   ///< force_update cycle — update without comparing
    SkipNoAddress,  ///< No usable local address — do not call the driver
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
        return UpdateDecision::SkipNoAddress;
    }
    if (force_update) {
        return UpdateDecision::UpdateForced;
    }
    if (!current_records.empty() && current_records.front() == *local_address) {
        return UpdateDecision::SkipUnchanged;
    }
    return UpdateDecision::UpdateChanged;
}

} // namespace domain

#endif // YADDNSC_DOMAIN_UPDATE_UPDATE_DECISION_H
