//
// Unit tests for src/domain/driver/capabilities.h — the DriverCapabilities
// value type.
//
// Verifies:
//   - An empty capability set is legal and supports no record kind.
//   - A listed record kind is supported; an unlisted one is not.
//   - The supported record kinds are readable and keep the given order.
//   - The capability set owns its record kinds and copies independently.
// =============================================================================

#include "domain/driver/capabilities.h"

#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "domain/dns/record_kind.h"

namespace {

/// A capability set built from the given record kinds.
[[nodiscard]] domain::DriverCapabilities make(std::vector<domain::RecordKind> kinds) {
    return domain::DriverCapabilities(std::move(kinds));
}

}  // anonymous namespace

TEST(DriverCapabilitiesTest, EmptySetSupportsNoRecordKind) {
    const domain::DriverCapabilities capabilities;

    EXPECT_TRUE(capabilities.supported_records().empty());
    EXPECT_FALSE(capabilities.supports(domain::RecordKind::A));
    EXPECT_FALSE(capabilities.supports(domain::RecordKind::AAAA));
    EXPECT_FALSE(capabilities.supports(domain::RecordKind::TXT));
}

TEST(DriverCapabilitiesTest, EmptyListIsLegal) {
    // A driver that advertises no record type still loads; the configuration
    // naming one is rejected later, not here.
    const auto capabilities = make({});

    EXPECT_TRUE(capabilities.supported_records().empty());
    EXPECT_FALSE(capabilities.supports(domain::RecordKind::A));
}

TEST(DriverCapabilitiesTest, SupportsOnlyTheListedKind) {
    const auto capabilities = make({domain::RecordKind::A});

    EXPECT_TRUE(capabilities.supports(domain::RecordKind::A));
    EXPECT_FALSE(capabilities.supports(domain::RecordKind::AAAA));
    EXPECT_FALSE(capabilities.supports(domain::RecordKind::TXT));
}

TEST(DriverCapabilitiesTest, SupportsEveryListedKind) {
    const auto capabilities = make({domain::RecordKind::A, domain::RecordKind::AAAA, domain::RecordKind::TXT});

    EXPECT_TRUE(capabilities.supports(domain::RecordKind::A));
    EXPECT_TRUE(capabilities.supports(domain::RecordKind::AAAA));
    EXPECT_TRUE(capabilities.supports(domain::RecordKind::TXT));
}

TEST(DriverCapabilitiesTest, ReportsTheSupportedKindsInOrder) {
    const auto capabilities = make({domain::RecordKind::AAAA, domain::RecordKind::A});

    const auto& records = capabilities.supported_records();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0], domain::RecordKind::AAAA);
    EXPECT_EQ(records[1], domain::RecordKind::A);
}

TEST(DriverCapabilitiesTest, OwnsItsRecordKinds) {
    // The caller's list is moved into the value; the set keeps its own copy.
    std::vector<domain::RecordKind> kinds{domain::RecordKind::A};
    const auto capabilities = domain::DriverCapabilities(std::move(kinds));

    ASSERT_EQ(capabilities.supported_records().size(), 1U);
    EXPECT_EQ(capabilities.supported_records().front(), domain::RecordKind::A);
}

TEST(DriverCapabilitiesTest, CopyKeepsTheSupportedKinds) {
    const auto original = make({domain::RecordKind::AAAA});
    const auto copy = original;

    EXPECT_EQ(copy.supported_records(), original.supported_records());
    EXPECT_TRUE(copy.supports(domain::RecordKind::AAAA));
    EXPECT_FALSE(copy.supports(domain::RecordKind::A));
}
