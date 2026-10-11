//
// Unit tests for dns/wire/framing.h — the classic TCP length-prefix codec.
//
// Tests cover:
//   - frame_message: prefix encoding, the 65535-octet protocol ceiling
//   - read_length: zero length, the MAX_RESPONSE_SIZE acceptance bound
// =============================================================================

#include "infrastructure/dns/wire/framing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

/// Frame a message of @p payload_size zero octets.
[[nodiscard]] std::vector<std::uint8_t> zeros(const std::size_t payload_size) {
    return std::vector<std::uint8_t>(payload_size, 0);
}

// ===========================================================================
//  frame_message
// ===========================================================================

TEST(DnsFraming, frame_message_PrependsTheBigEndianLength) {
    const std::vector<std::uint8_t> payload{0x12, 0x34, 0x56};

    const auto framed = dns::frame_message(payload);

    ASSERT_TRUE(framed.has_value());
    ASSERT_EQ(framed->size(), 5U);
    EXPECT_EQ((*framed)[0], 0x00);
    EXPECT_EQ((*framed)[1], 0x03);
    EXPECT_EQ((*framed)[2], 0x12);
}

TEST(DnsFraming, frame_message_AcceptsTheProtocolCeiling) {
    EXPECT_TRUE(dns::frame_message(zeros(65535)).has_value());
}

TEST(DnsFraming, frame_message_RejectsBeyondTheProtocolCeiling) {
    const auto framed = dns::frame_message(zeros(65536));

    ASSERT_FALSE(framed.has_value());
    EXPECT_EQ(framed.error(), dns::FrameError::MESSAGE_TOO_LARGE);
}

// ===========================================================================
//  read_length
// ===========================================================================

/// The two prefix octets of a big-endian 16-bit @p length.
[[nodiscard]] std::array<std::uint8_t, 2> prefix_of(const std::size_t length) {
    return {static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length & 0xFF)};
}

TEST(DnsFraming, read_length_RejectsZero) {
    const auto prefix = prefix_of(0);

    const auto length = dns::read_length(prefix);

    ASSERT_FALSE(length.has_value());
    EXPECT_EQ(length.error(), dns::FrameError::INVALID_LENGTH);
}

TEST(DnsFraming, read_length_AcceptsTheSmallestMessage) {
    const auto prefix = prefix_of(1);

    const auto length = dns::read_length(prefix);

    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(*length, 1U);
}

TEST(DnsFraming, read_length_AcceptsTheResponseBound) {
    const auto prefix = prefix_of(dns::MAX_RESPONSE_SIZE);

    const auto length = dns::read_length(prefix);

    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(*length, dns::MAX_RESPONSE_SIZE);
}

TEST(DnsFraming, read_length_RejectsOnePastTheResponseBound) {
    const auto prefix = prefix_of(dns::MAX_RESPONSE_SIZE + 1);

    const auto length = dns::read_length(prefix);

    ASSERT_FALSE(length.has_value());
    EXPECT_EQ(length.error(), dns::FrameError::MESSAGE_TOO_LARGE);
}

TEST(DnsFraming, read_length_RejectsTheProtocolCeiling) {
    // 65535 is encodable in the prefix but past the accepted response bound.
    const auto prefix = prefix_of(65535);

    const auto length = dns::read_length(prefix);

    ASSERT_FALSE(length.has_value());
    EXPECT_EQ(length.error(), dns::FrameError::MESSAGE_TOO_LARGE);
}

}  // namespace
