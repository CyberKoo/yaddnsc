//
// UpdateDecision unit tests — domain::decide_update is a pure function over
// already-fetched inputs.
//
// Decision policy:
//   - no local address             → SKIP_NO_ADDRESS (driver never called)
//   - force_update                 → UPDATE_FORCED (DNS not consulted)
//   - non-empty RRset, every record == local → SKIP_UNCHANGED
//   - everything else (incl. empty record list, however it arose) → UPDATE_CHANGED
// The comparison is an all_of over the whole RRset: one record differing from
// local is enough to update.
//

#include "domain/update/update_decision.h"

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

using domain::decide_update;
using domain::UpdateDecision;

const std::optional<std::string> LOCAL{"198.51.100.1"};

// ── no local address → SKIP_NO_ADDRESS ─────────────────────────────────────────

TEST(UpdateDecision, NoLocalAddressSkipsWithoutDriverCall) {
    EXPECT_EQ(decide_update({"192.0.2.1"}, std::nullopt, false), UpdateDecision::SKIP_NO_ADDRESS);
}

TEST(UpdateDecision, NoLocalAddressWinsOverForceUpdate) {
    // Step order matters: without a local address there is nothing to publish,
    // so even a forced cycle skips.
    EXPECT_EQ(decide_update({}, std::nullopt, true), UpdateDecision::SKIP_NO_ADDRESS);
}

// ── force_update → UPDATE_FORCED ──────────────────────────────────────────────

TEST(UpdateDecision, ForceUpdateIgnoresDnsRecords) {
    EXPECT_EQ(decide_update({"198.51.100.1"}, LOCAL, true), UpdateDecision::UPDATE_FORCED);
    EXPECT_EQ(decide_update({}, LOCAL, true), UpdateDecision::UPDATE_FORCED);
}

// ── all records equal local → SKIP_UNCHANGED ──────────────────────────────────

TEST(UpdateDecision, FirstRecordMatchingLocalSkips) {
    EXPECT_EQ(decide_update({"198.51.100.1"}, LOCAL, false), UpdateDecision::SKIP_UNCHANGED);
}

TEST(UpdateDecision, MatchingSiblingDoesNotHideAStaleManagedRecord) {
    // DNS cannot say which address belongs to the configured record_id.
    // Neither answer order may cause a stale managed record to be skipped.
    EXPECT_EQ(decide_update({"203.0.113.9", "198.51.100.1"}, LOCAL, false), UpdateDecision::UPDATE_CHANGED);
    EXPECT_EQ(decide_update({"198.51.100.1", "203.0.113.9"}, LOCAL, false), UpdateDecision::UPDATE_CHANGED);
}

TEST(UpdateDecision, AllMatchingRecordsSkip) {
    EXPECT_EQ(decide_update({"198.51.100.1", "198.51.100.1"}, LOCAL, false), UpdateDecision::SKIP_UNCHANGED);
}

TEST(UpdateDecision, NoMatchingRecordUpdates) {
    EXPECT_EQ(decide_update({"203.0.113.9", "192.0.2.1"}, LOCAL, false), UpdateDecision::UPDATE_CHANGED);
}

// ── everything else → UPDATE_CHANGED ──────────────────────────────────────────

TEST(UpdateDecision, DifferentFirstRecordUpdates) {
    EXPECT_EQ(decide_update({"192.0.2.1"}, LOCAL, false), UpdateDecision::UPDATE_CHANGED);
}

TEST(UpdateDecision, EmptyRecordListUpdates) {
    // A successful-but-empty answer and a failed lookup mapped to empty are
    // the same decision: update. There is no "skip because no record" state.
    EXPECT_EQ(decide_update({}, LOCAL, false), UpdateDecision::UPDATE_CHANGED);
}

}  // namespace
