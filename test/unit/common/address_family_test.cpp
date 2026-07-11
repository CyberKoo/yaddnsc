//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for address_family.h — AddressFamily enum.
//
// Verifies:
//   - All enumerator values are defined.
//   - enum class semantics prevent implicit conversion.
// =============================================================================

#include <type_traits>

#include <gtest/gtest.h>

#include "address_family.h"

TEST(AddressFamilyTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(AddressFamily::UNSPECIFIED), 0);
    EXPECT_EQ(static_cast<int>(AddressFamily::IPV4), 1);
    EXPECT_EQ(static_cast<int>(AddressFamily::IPV6), 2);
}

TEST(AddressFamilyTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<AddressFamily>));
    EXPECT_FALSE((std::is_convertible_v<AddressFamily, int>));
}

TEST(AddressFamilyTest, Unspecified_IsDefault) {
    AddressFamily af{};
    EXPECT_EQ(af, AddressFamily::UNSPECIFIED);
}

TEST(AddressFamilyTest, Switch_CoversAllValues) {
    auto classify = [](AddressFamily af) -> const char* {
        switch (af) {
            case AddressFamily::UNSPECIFIED: return "unspec";
            case AddressFamily::IPV4:        return "v4";
            case AddressFamily::IPV6:        return "v6";
        }
        return "unknown";
    };

    EXPECT_STREQ(classify(AddressFamily::UNSPECIFIED), "unspec");
    EXPECT_STREQ(classify(AddressFamily::IPV4), "v4");
    EXPECT_STREQ(classify(AddressFamily::IPV6), "v6");
}
