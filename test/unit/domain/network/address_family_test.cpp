// Unit tests for address_family.h — AddressFamily enum.
//
// Verifies:
//   - All enumerator values are defined.
//   - enum class semantics prevent implicit conversion.
// =============================================================================

#include "domain/network/address_family.h"

#include <type_traits>

#include <gtest/gtest.h>

TEST(AddressFamilyTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(domain::AddressFamily::UNSPECIFIED), 0);
    EXPECT_EQ(static_cast<int>(domain::AddressFamily::IPV4), 1);
    EXPECT_EQ(static_cast<int>(domain::AddressFamily::IPV6), 2);
}

TEST(AddressFamilyTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<domain::AddressFamily>) );
    EXPECT_FALSE((std::is_convertible_v<domain::AddressFamily, int>) );
}

TEST(AddressFamilyTest, Unspecified_IsDefault) {
    domain::AddressFamily af{};
    EXPECT_EQ(af, domain::AddressFamily::UNSPECIFIED);
}

TEST(AddressFamilyTest, Switch_CoversAllValues) {
    auto classify = [](domain::AddressFamily af) -> const char* {
        switch (af) {
            case domain::AddressFamily::UNSPECIFIED:
                return "unspec";
            case domain::AddressFamily::IPV4:
                return "v4";
            case domain::AddressFamily::IPV6:
                return "v6";
        }
        return "unknown";
    };

    EXPECT_STREQ(classify(domain::AddressFamily::UNSPECIFIED), "unspec");
    EXPECT_STREQ(classify(domain::AddressFamily::IPV4), "v4");
    EXPECT_STREQ(classify(domain::AddressFamily::IPV6), "v6");
}
