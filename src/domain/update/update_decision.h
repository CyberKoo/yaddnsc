#ifndef YADDNSC_DOMAIN_UPDATE_UPDATE_DECISION_H
#define YADDNSC_DOMAIN_UPDATE_UPDATE_DECISION_H

#include <optional>
#include <string>
#include <vector>

/// UpdateDecision — pure decision over already-fetched inputs for one update
/// cycle.
///
/// Decision policy:
///   - no local address          → SKIP_NO_ADDRESS (the driver is never called);
///   - force_update              → UPDATE_FORCED (DNS records are not consulted);
///   - a non-empty RRset with EVERY record equal to the local address → SKIP_UNCHANGED;
///   - everything else — including an empty record list, whether the lookup
///     succeeded empty or failed and was mapped to empty — is UPDATE_CHANGED.
/// DNS cannot identify the provider's managed record_id. A matching sibling
/// must not hide a stale managed record, so mixed-address RRsets are updated
/// conservatively even when one address matches. No "skip because no record"
/// state exists.
namespace domain {

enum class UpdateDecision {
    SKIP_UNCHANGED,   ///< All returned DNS records equal the local address
    UPDATE_CHANGED,   ///< Records differ (or are unavailable) — update
    UPDATE_FORCED,    ///< force_update cycle — update without comparing
    SKIP_NO_ADDRESS,  ///< No usable local address — do not call the driver
};

/// Decide what one update cycle should do.
///
/// @param current_records  DNS records to compare against; a failed lookup
///                         arrives as an empty list (mapped by the workflow).
/// @param local_address    The address to publish, if any was resolved.
/// @param force_update     Skip the comparison and update unconditionally.
[[nodiscard]] UpdateDecision decide_update(const std::vector<std::string>& current_records,
                                           const std::optional<std::string>& local_address, bool force_update) noexcept;

}  // namespace domain

#endif  // YADDNSC_DOMAIN_UPDATE_UPDATE_DECISION_H
