//
// Unit tests for the YADDNSC_DRIVER_MAGIC constant in the v1 alpha plugin
// ABI header (<yaddnsc/sdk/driver_abi.h>).
//
// Verifies:
//   - The value is a 64-bit unsigned compile-time constant.
//   - The value is the expected ASCII encoding of "YADDNSC\0".
//   - The constant is non-zero (provides a useful validity check).
// =============================================================================

#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include <yaddnsc/sdk/driver_abi.h>

TEST(DriverMagicTest, TypeIs64BitUnsigned) {
    static_assert(sizeof(decltype(YADDNSC_DRIVER_MAGIC)) == sizeof(std::uint64_t));
    EXPECT_TRUE((std::is_unsigned_v<decltype(YADDNSC_DRIVER_MAGIC)>));
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
    // The macro expands to a literal, usable in any constant expression.
    constexpr auto magic = YADDNSC_DRIVER_MAGIC;
    EXPECT_EQ(magic, 0x594144444E534300ULL);
}

TEST(DriverMagicTest, Constexpr_StaticAssert) {
    static_assert(YADDNSC_DRIVER_MAGIC == 0x594144444E534300ULL);
    static_assert(YADDNSC_DRIVER_MAGIC != 0);
}
