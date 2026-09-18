//
// UpdateDecision unit tests — domain::decide_update is a pure function over
// already-fetched inputs.
//
// Locked behaviours (legacy Updater semantics):
//   - no local address             → SKIP_NO_ADDRESS (driver never called)
//   - force_update                 → UPDATE_FORCED (DNS not consulted)
//   - FIRST record == local        → SKIP_UNCHANGED
//   - everything else (incl. empty record list, however it arose) → UPDATE_CHANGED
// Only the first record participates in the comparison.
//

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "domain/update/update_decision.h"

namespace {

using domain::UpdateDecision;
using domain::decide_update;

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

// ── first record equals local → SKIP_UNCHANGED ────────────────────────────────

TEST(UpdateDecision, FirstRecordMatchingLocalSkips) {
    EXPECT_EQ(decide_update({"198.51.100.1"}, LOCAL, false), UpdateDecision::SKIP_UNCHANGED);
}

TEST(UpdateDecision, OnlyTheFirstRecordIsCompared) {
    // The second record differs from the local address; it must not matter.
    EXPECT_EQ(decide_update({"198.51.100.1", "203.0.113.9"}, LOCAL, false), UpdateDecision::SKIP_UNCHANGED);
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

} // namespace
