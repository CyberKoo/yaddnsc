//
// Unit tests for src/domain/dns/record_kind.h — RecordKind enum.
//
// Verifies:
//   - All enumerator values are defined and stable.
//   - enum class semantics prevent implicit conversion.
//   - Default-initialised value is A (first enumerator).
//   - All three values are distinct.
// =============================================================================

#include "domain/dns/record_kind.h"

#include <type_traits>

#include <gtest/gtest.h>

TEST(RecordKindTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(domain::RecordKind::A), 0);
    EXPECT_EQ(static_cast<int>(domain::RecordKind::AAAA), 1);
    EXPECT_EQ(static_cast<int>(domain::RecordKind::TXT), 2);
}

TEST(RecordKindTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<domain::RecordKind>) );
    EXPECT_FALSE((std::is_convertible_v<domain::RecordKind, int>) );
}

TEST(RecordKindTest, DefaultIsA) {
    domain::RecordKind rk{};
    EXPECT_EQ(rk, domain::RecordKind::A);
}

TEST(RecordKindTest, AllValues_Distinct) {
    EXPECT_NE(domain::RecordKind::A, domain::RecordKind::AAAA);
    EXPECT_NE(domain::RecordKind::A, domain::RecordKind::TXT);
    EXPECT_NE(domain::RecordKind::AAAA, domain::RecordKind::TXT);
}

TEST(RecordKindTest, Switch_CoversAllValues) {
    auto classify = [](domain::RecordKind rk) -> const char* {
        switch (rk) {
            case domain::RecordKind::A:
                return "A";
            case domain::RecordKind::AAAA:
                return "AAAA";
            case domain::RecordKind::TXT:
                return "TXT";
        }
        return "unknown";
    };

    EXPECT_STREQ(classify(domain::RecordKind::A), "A");
    EXPECT_STREQ(classify(domain::RecordKind::AAAA), "AAAA");
    EXPECT_STREQ(classify(domain::RecordKind::TXT), "TXT");
}
