//
// UpdateDecision unit tests — domain::decide_update is a pure function over
// already-fetched inputs (refactor/phase-3-scheduling-and-workflow.md §3.3).
//
// Locked behaviours (Phase 0 table):
//   - no local address             → SkipNoAddress (driver never called)
//   - force_update                 → UpdateForced (DNS not consulted)
//   - FIRST record == local        → SkipUnchanged
//   - everything else (incl. empty record list, however it arose) → UpdateChanged
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

// ── no local address → SkipNoAddress ─────────────────────────────────────────

TEST(UpdateDecision, NoLocalAddressSkipsWithoutDriverCall) {
    EXPECT_EQ(decide_update({"192.0.2.1"}, std::nullopt, false), UpdateDecision::SkipNoAddress);
}

TEST(UpdateDecision, NoLocalAddressWinsOverForceUpdate) {
    // Step order matters: without a local address there is nothing to publish,
    // so even a forced cycle skips.
    EXPECT_EQ(decide_update({}, std::nullopt, true), UpdateDecision::SkipNoAddress);
}

// ── force_update → UpdateForced ──────────────────────────────────────────────

TEST(UpdateDecision, ForceUpdateIgnoresDnsRecords) {
    EXPECT_EQ(decide_update({"198.51.100.1"}, LOCAL, true), UpdateDecision::UpdateForced);
    EXPECT_EQ(decide_update({}, LOCAL, true), UpdateDecision::UpdateForced);
}

// ── first record equals local → SkipUnchanged ────────────────────────────────

TEST(UpdateDecision, FirstRecordMatchingLocalSkips) {
    EXPECT_EQ(decide_update({"198.51.100.1"}, LOCAL, false), UpdateDecision::SkipUnchanged);
}

TEST(UpdateDecision, OnlyTheFirstRecordIsCompared) {
    // The second record differs from the local address; it must not matter.
    EXPECT_EQ(decide_update({"198.51.100.1", "203.0.113.9"}, LOCAL, false), UpdateDecision::SkipUnchanged);
}

// ── everything else → UpdateChanged ──────────────────────────────────────────

TEST(UpdateDecision, DifferentFirstRecordUpdates) {
    EXPECT_EQ(decide_update({"192.0.2.1"}, LOCAL, false), UpdateDecision::UpdateChanged);
}

TEST(UpdateDecision, EmptyRecordListUpdates) {
    // A successful-but-empty answer and a failed lookup mapped to empty are
    // the same decision: update. There is no "skip because no record" state.
    EXPECT_EQ(decide_update({}, LOCAL, false), UpdateDecision::UpdateChanged);
}

} // namespace
