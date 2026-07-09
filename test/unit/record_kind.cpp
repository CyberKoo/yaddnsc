//
// Unit tests for include/record_kind.h — RecordKind enum.
//
// Verifies:
//   - All enumerator values are defined and stable.
//   - enum class semantics prevent implicit conversion.
//   - Default-initialised value is A (first enumerator).
//   - All three values are distinct.
// =============================================================================

#include <type_traits>

#include <gtest/gtest.h>

#include "record_kind.h"

TEST(RecordKindTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(RecordKind::A), 0);
    EXPECT_EQ(static_cast<int>(RecordKind::AAAA), 1);
    EXPECT_EQ(static_cast<int>(RecordKind::TXT), 2);
}

TEST(RecordKindTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<RecordKind>));
    EXPECT_FALSE((std::is_convertible_v<RecordKind, int>));
}

TEST(RecordKindTest, DefaultIsA) {
    RecordKind rk{};
    EXPECT_EQ(rk, RecordKind::A);
}

TEST(RecordKindTest, AllValues_Distinct) {
    EXPECT_NE(RecordKind::A, RecordKind::AAAA);
    EXPECT_NE(RecordKind::A, RecordKind::TXT);
    EXPECT_NE(RecordKind::AAAA, RecordKind::TXT);
}

TEST(RecordKindTest, Switch_CoversAllValues) {
    auto classify = [](RecordKind rk) -> const char* {
        switch (rk) {
            case RecordKind::A:    return "A";
            case RecordKind::AAAA: return "AAAA";
            case RecordKind::TXT:  return "TXT";
        }
        return "unknown";
    };

    EXPECT_STREQ(classify(RecordKind::A), "A");
    EXPECT_STREQ(classify(RecordKind::AAAA), "AAAA");
    EXPECT_STREQ(classify(RecordKind::TXT), "TXT");
}
