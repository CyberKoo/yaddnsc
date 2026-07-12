//
// Unit tests for include/driver/magic.h — YADDNSC_DRIVER_MAGIC constant.
//
// Verifies:
//   - The constant is defined with the correct type (std::uint64_t).
//   - The value is the expected ASCII encoding of "YADDNSC\0".
//   - The value is constexpr (compile-time constant).
//   - The constant is non-zero (provides a useful validity check).
// =============================================================================

#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "driver/magic.h"

TEST(DriverMagicTest, TypeIsUint64) {
    EXPECT_TRUE((std::is_same_v<decltype(YADDNSC_DRIVER_MAGIC), const std::uint64_t>));
}

TEST(DriverMagicTest, ValueIsNonZero) {
    EXPECT_NE(YADDNSC_DRIVER_MAGIC, 0ULL);
}

TEST(DriverMagicTest, Value_Matches_AsciiEncoding) {
    // "YADDNSC\0" in big-endian: Y=0x59, A=0x41, D=0x44, D=0x44, N=0x4E, S=0x53, C=0x43, \0=0x00
    // Packed as uint64_t: 0x594144444E534300
    EXPECT_EQ(YADDNSC_DRIVER_MAGIC, 0x594144444E534300ULL);
}

TEST(DriverMagicTest, Constexpr_Context) {
    // Verify it can be used in a constexpr context.
    constexpr auto magic = YADDNSC_DRIVER_MAGIC;
    EXPECT_EQ(magic, 0x594144444E534300ULL);
}

TEST(DriverMagicTest, Constexpr_StaticAssert) {
    // Compile-time check that the value is indeed constexpr.
    static_assert(YADDNSC_DRIVER_MAGIC == 0x594144444E534300ULL);
    static_assert(YADDNSC_DRIVER_MAGIC != 0);
}
