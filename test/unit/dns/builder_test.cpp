//
// Unit tests for dns/wire/builder.h — DNS::QueryBuilder.
//
// Tests cover:
//   - Header field encoding (ID, flags, counts)
//   - Question section (single, multiple, QTYPE, QCLASS)
//   - Domain name encoding (single label, multi-label, root, edge cases)
//   - Input validation (empty questions, label > 63, name > 255)
//   - EDNS0 OPT record (basic, options, validation)
//   - Raw QCLASS (mDNS QU bit)
// =============================================================================

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#include <gtest/gtest.h>

#include "dns/wire/builder.h"
#include "exception/dns_packet.h"

// ===========================================================================
//  Helper: decode a 2-byte big-endian value at offset
// ===========================================================================

namespace {

    [[nodiscard]] std::uint16_t read_u16(const std::vector<std::uint8_t>& buf, size_t offset) {
        return static_cast<std::uint16_t>((buf[offset] << 8) | buf[offset + 1]);
    }

    [[nodiscard]] std::uint32_t read_u32(const std::vector<std::uint8_t>& buf, size_t offset) {
        return (static_cast<std::uint32_t>(buf[offset])     << 24)
             | (static_cast<std::uint32_t>(buf[offset + 1]) << 16)
             | (static_cast<std::uint32_t>(buf[offset + 2]) << 8)
             |  static_cast<std::uint32_t>(buf[offset + 3]);
    }

    /// Verify the 12-byte DNS header matches expected values.
    void expect_header(const std::vector<std::uint8_t>& packet,
                        std::uint16_t expected_id,
                        std::uint16_t expected_flags,
                        std::uint16_t expected_qdcount,
                        std::uint16_t expected_ancount,
                        std::uint16_t expected_nscount,
                        std::uint16_t expected_arcount) {
        ASSERT_GE(packet.size(), 12U);
        EXPECT_EQ(read_u16(packet, 0), expected_id);
        EXPECT_EQ(read_u16(packet, 2), expected_flags);
        EXPECT_EQ(read_u16(packet, 4), expected_qdcount);
        EXPECT_EQ(read_u16(packet, 6), expected_ancount);
        EXPECT_EQ(read_u16(packet, 8), expected_nscount);
        EXPECT_EQ(read_u16(packet, 10), expected_arcount);
    }

    /// Verify "example.com" QNAME at the given offset.
    void expect_qname_example_com(const std::vector<std::uint8_t>& packet, size_t offset) {
        ASSERT_GE(packet.size(), offset + 13);
        EXPECT_EQ(packet[offset + 0], 7);
        EXPECT_EQ(packet[offset + 1], 'e');
        EXPECT_EQ(packet[offset + 2], 'x');
        EXPECT_EQ(packet[offset + 3], 'a');
        EXPECT_EQ(packet[offset + 4], 'm');
        EXPECT_EQ(packet[offset + 5], 'p');
        EXPECT_EQ(packet[offset + 6], 'l');
        EXPECT_EQ(packet[offset + 7], 'e');
        EXPECT_EQ(packet[offset + 8], 3);
        EXPECT_EQ(packet[offset + 9], 'c');
        EXPECT_EQ(packet[offset + 10], 'o');
        EXPECT_EQ(packet[offset + 11], 'm');
        EXPECT_EQ(packet[offset + 12], 0);
    }

}  // anonymous namespace

// ===========================================================================
//  Default / basic queries
// ===========================================================================

TEST(QueryBuilderTest, DefaultQueryHasRandomIdAndRd) {
    DNS::QueryBuilder builder;
    builder.add_question("example.com", DNS::RecordType::A);
    auto packet = builder.build();

    // Default: QR=0, OPCODE=0, RD=1 → flags = 0x0100
    expect_header(packet, read_u16(packet, 0), 0x0100, 1, 0, 0, 0);
    EXPECT_GT(packet.size(), 12U);
}

TEST(QueryBuilderTest, BuildsExampleComA) {
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .build();

    expect_qname_example_com(packet, 12);

    // QTYPE = A (1)
    EXPECT_EQ(read_u16(packet, 25), 1);

    // QCLASS = IN (1)
    EXPECT_EQ(read_u16(packet, 27), 1);

    // Total size: header(12) + qname(13) + qtype(2) + qclass(2) = 29
    EXPECT_EQ(packet.size(), 29U);
}

TEST(QueryBuilderTest, BuildsAaaaRecord) {
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::AAAA)
        .build();

    // QTYPE = AAAA (28) = 0x001C
    EXPECT_EQ(read_u16(packet, 25), 28);
}

// ===========================================================================
//  Custom header fields
// ===========================================================================

TEST(QueryBuilderTest, CustomId) {
    auto packet = DNS::QueryBuilder{}
        .id(0xABCD)
        .add_question("example.com", DNS::RecordType::A)
        .build();

    EXPECT_EQ(read_u16(packet, 0), 0xABCD);
}

TEST(QueryBuilderTest, QrFlag) {
    auto packet = DNS::QueryBuilder{}
        .qr(true)
        .add_question("example.com", DNS::RecordType::A)
        .build();

    // QR = bit 15 → 0x8000 | RD = 0x0100 → 0x8100
    EXPECT_EQ(read_u16(packet, 2), 0x8100);
}

TEST(QueryBuilderTest, AllFlagsSet) {
    auto packet = DNS::QueryBuilder{}
        .qr(true)
        .aa(true)
        .tc(true)
        .rd(true)
        .ra(true)
        .add_question("example.com", DNS::RecordType::A)
        .build();

    // QR(0x8000) | AA(0x0400) | TC(0x0200) | RD(0x0100) | RA(0x0080) = 0x8780
    EXPECT_EQ(read_u16(packet, 2), 0x8780);
}

TEST(QueryBuilderTest, OpcodeSetsBits) {
    auto packet = DNS::QueryBuilder{}
        .opcode(1)  // Inverse query (RFC 1035)
        .add_question("example.com", DNS::RecordType::A)
        .build();

    // OPCODE=1 << 11 = 0x0800 | RD=1 = 0x0100 → flags = 0x0900
    EXPECT_EQ(read_u16(packet, 2), 0x0900);
}

TEST(QueryBuilderTest, QrAndRdControl) {
    // QR=0, RD=1 (default query)
    auto query = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .build();
    EXPECT_EQ(read_u16(query, 2), 0x0100);

    // QR=1, RD=0 (response)
    auto resp = DNS::QueryBuilder{}
        .qr(true)
        .rd(false)
        .add_question("example.com", DNS::RecordType::A)
        .build();
    EXPECT_EQ(read_u16(resp, 2), 0x8000);
}

// ===========================================================================
//  Multiple questions
// ===========================================================================

TEST(QueryBuilderTest, MultipleQuestions) {
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .add_question("example.com", DNS::RecordType::AAAA)
        .build();

    // QDCOUNT = 2
    EXPECT_EQ(read_u16(packet, 4), 2);

    // ANCOUNT = NSCOUNT = ARCOUNT = 0
    EXPECT_EQ(read_u16(packet, 6), 0);
    EXPECT_EQ(read_u16(packet, 8), 0);
    EXPECT_EQ(read_u16(packet, 10), 0);

    // First question: A record starting at offset 12
    expect_qname_example_com(packet, 12);
    EXPECT_EQ(read_u16(packet, 25), 1);  // QTYPE = A

    // Second question: AAAA record, after first QNAME(13) + QTYPE(2) + QCLASS(2) = 17 bytes
    size_t q2_offset = 12 + 13 + 2 + 2;  // = 29
    expect_qname_example_com(packet, q2_offset);
    EXPECT_EQ(read_u16(packet, q2_offset + 13), 28);  // QTYPE = AAAA
}

TEST(QueryBuilderTest, MultipleDifferentNames) {
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .add_question("google.com", DNS::RecordType::AAAA)
        .build();

    EXPECT_EQ(read_u16(packet, 4), 2);

    // First QNAME: \x07example\x03com\x00 (13 bytes, offset 12)
    // First question size: 13 + QTYPE(2) + QCLASS(2) = 17 bytes
    // Second QNAME starts at offset 12 + 17 = 29
    // Second QNAME: \x06google\x03com\x00 (12 bytes: 6+1 + 3+1 + 1 root)
    size_t q2_qname_offset = 29;
    size_t q2_qname_len = 12;
    ASSERT_GE(packet.size(), q2_qname_offset + q2_qname_len + 4);
    EXPECT_EQ(packet[q2_qname_offset], 6);
    EXPECT_EQ(packet[q2_qname_offset + 1], 'g');

    // QTYPE at offset after QNAME = 29 + 12 = 41
    EXPECT_EQ(read_u16(packet, q2_qname_offset + q2_qname_len), 28) << "QTYPE should be AAAA";

    // QCLASS = IN at offset 43
    EXPECT_EQ(read_u16(packet, q2_qname_offset + q2_qname_len + 2), 1) << "QCLASS should be IN";
}

// ===========================================================================
//  Domain name encoding edge cases
// ===========================================================================

TEST(QueryBuilderTest, SingleLabelName) {
    auto packet = DNS::QueryBuilder{}
        .add_question("localhost", DNS::RecordType::A)
        .build();

    // \x09localhost\x00
    ASSERT_GE(packet.size(), 12 + 11);
    EXPECT_EQ(packet[12], 9);
    EXPECT_EQ(packet[13], 'l');
    EXPECT_EQ(packet[14], 'o');
    EXPECT_EQ(packet[15], 'c');
    EXPECT_EQ(packet[16], 'a');
    EXPECT_EQ(packet[17], 'l');
    EXPECT_EQ(packet[18], 'h');
    EXPECT_EQ(packet[19], 'o');
    EXPECT_EQ(packet[20], 's');
    EXPECT_EQ(packet[21], 't');
    EXPECT_EQ(packet[22], 0);
}

TEST(QueryBuilderTest, BareDotEncodesAsRoot) {
    // A single dot is the root domain — should encode as a single \x00 byte
    // inside the query, same as an empty name.
    auto packet = DNS::QueryBuilder{}
        .id(0)
        .rd(false)
        .add_question(".", DNS::RecordType::NS)
        .build();

    // QNAME starts at offset 12; root is a single zero byte.
    EXPECT_EQ(packet[12], 0) << "Root label should be a single zero byte";
    // QTYPE at offset 13, QCLASS at offset 15
    EXPECT_EQ(read_u16(packet, 13), 2);   // NS
    // Name length = 1 (root) + 2 (QTYPE) + 2 (QCLASS) = 5
    EXPECT_EQ(packet.size(), 17U);
}

TEST(QueryBuilderTest, TrailingDot) {
    // "example.com." with trailing dot should encode the same way.
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com.", DNS::RecordType::A)
        .build();

    // Should produce same encoding as "example.com" (without trailing dot)
    // QNAME: \x07example\x03com\x00
    EXPECT_EQ(packet[12], 7);
    EXPECT_EQ(packet[20], 3);
    EXPECT_EQ(packet[24], 0);
    EXPECT_EQ(packet.size(), 29U);
}

// ===========================================================================
//  Raw QCLASS (mDNS QU bit)
// ===========================================================================

TEST(QueryBuilderTest, RawQclassSetsQuBit) {
    auto packet = DNS::QueryBuilder{}
        .id(0)
        .rd(false)
        .add_question_raw_qclass("local", DNS::RecordType::A,
            static_cast<std::uint16_t>(DNS::RecordClass::IN) | 0x8000)
        .build();

    // QNAME: \x05local\x00 (7 bytes, offset 12)
    // QTYPE at offset 19, QCLASS at offset 21
    EXPECT_EQ(read_u16(packet, 21), 0x8001);  // IN | QU
}

// ===========================================================================
//  EDNS0
// ===========================================================================

TEST(QueryBuilderTest, EdnsSetsArcount) {
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .add_edns(4096, 0, false)
        .build();

    // ARCOUNT should be 1 with EDNS0
    EXPECT_EQ(read_u16(packet, 10), 1);
}

TEST(QueryBuilderTest, EdnsOptRecordFields) {
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .add_edns(1232, 0, true)
        .build();

    // After the question section: header(12) + qname(13) + qtype(2) + qclass(2) = 29
    size_t opt_offset = 29;

    // OPT name: root label (1 byte: 0x00)
    EXPECT_EQ(packet[opt_offset], 0);

    // OPT type: 41 (0x0029)
    EXPECT_EQ(read_u16(packet, opt_offset + 1), 41);

    // OPT class: UDP payload size = 1232 (0x04D0)
    EXPECT_EQ(read_u16(packet, opt_offset + 3), 1232);

    // OPT TTL: version=0, DO=1 → bits 23-16 = 0, bit 15 = 1 → 0x00008000
    EXPECT_EQ(read_u32(packet, opt_offset + 5), 0x00008000U);

    // RDLENGTH = 0 (no options)
    EXPECT_EQ(read_u16(packet, opt_offset + 9), 0);
}

TEST(QueryBuilderTest, EdnsWithoutDnssec) {
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .add_edns(4096, 0, false)
        .build();

    size_t opt_offset = 29;

    // TTL: version=0, DO=0 → 0x00000000
    EXPECT_EQ(read_u16(packet, opt_offset + 5), 0);
    EXPECT_EQ(read_u16(packet, opt_offset + 7), 0);
}

TEST(QueryBuilderTest, EdnsWithOptions) {
    std::vector<DNS::EdnsOption> opts = {
        {1, {0x00, 0x08}},                         // code=1, 2 bytes of data
        {2, {0xAA, 0xBB, 0xCC}},                   // code=2, 3 bytes of data
    };

    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .add_edns(4096, 0, false, opts)
        .build();

    size_t opt_offset = 29;

    // RDLENGTH = (2+2+2) + (2+2+3) = 6 + 7 = 13
    EXPECT_EQ(read_u16(packet, opt_offset + 9), 13);

    // Option 1: code=1, length=2, data=[0x00, 0x08]
    EXPECT_EQ(read_u16(packet, opt_offset + 11), 1);
    EXPECT_EQ(read_u16(packet, opt_offset + 13), 2);
    EXPECT_EQ(packet[opt_offset + 15], 0x00);
    EXPECT_EQ(packet[opt_offset + 16], 0x08);

    // Option 2: code=2, length=3, data=[0xAA, 0xBB, 0xCC]
    EXPECT_EQ(read_u16(packet, opt_offset + 17), 2);
    EXPECT_EQ(read_u16(packet, opt_offset + 19), 3);
    EXPECT_EQ(packet[opt_offset + 21], 0xAA);
    EXPECT_EQ(packet[opt_offset + 22], 0xBB);
    EXPECT_EQ(packet[opt_offset + 23], 0xCC);
}

// ===========================================================================
//  Input validation — exception tests
// ===========================================================================

TEST(QueryBuilderTest, ThrowsOnEmptyQuestion) {
    DNS::QueryBuilder builder;
    EXPECT_THROW(
        {
            [[maybe_unused]] auto _ = builder.build();
        },
        DnsPacketException);
}

TEST(QueryBuilderTest, ThrowsOnLabelTooLong) {
    // A single label of 64 characters exceeds the 63-octet limit.
    std::string long_label(64, 'a');
    DNS::QueryBuilder builder;
    builder.add_question(long_label, DNS::RecordType::A);
    EXPECT_THROW(
        {
            [[maybe_unused]] auto _ = builder.build();
        },
        DnsPacketException);
}

TEST(QueryBuilderTest, AcceptsLabelLength63) {
    // Maximum valid label length: 63 characters
    std::string max_label(63, 'a');
    DNS::QueryBuilder builder;
    builder.add_question(max_label + ".com", DNS::RecordType::A);
    EXPECT_NO_THROW(
        {
            [[maybe_unused]] auto _ = builder.build();
        });
}

TEST(QueryBuilderTest, ThrowsOnNameTooLong) {
    // Build a name that exceeds 255 octets when encoded.
    std::string long_name;
    for (int i = 0; i < 6; ++i) {
        if (i > 0) long_name += '.';
        long_name.append(50, 'a');
    }
    DNS::QueryBuilder builder;
    builder.add_question(long_name, DNS::RecordType::A);
    EXPECT_THROW(
        {
            [[maybe_unused]] auto _ = builder.build();
        },
        DnsPacketException);
}

TEST(QueryBuilderTest, AcceptsMaxNameLength) {
    // Build a name whose encoded form fits within the 255-octet limit.
    // Label data: 62+62+62+61 = 247 bytes; 3 dots in the string form.
    // Encoded: 247 (data) + 4 (length bytes) + 1 (terminator) = 252.
    std::string max_name;
    max_name.append(62, 'a'); max_name += '.';
    max_name.append(62, 'a'); max_name += '.';
    max_name.append(62, 'a'); max_name += '.';
    max_name.append(61, 'a');
    ASSERT_EQ(max_name.size(), 250U);

    DNS::QueryBuilder builder;
    builder.add_question(max_name, DNS::RecordType::A);
    EXPECT_NO_THROW(
        {
            [[maybe_unused]] auto _ = builder.build();
        });
}

TEST(QueryBuilderTest, ThrowsOnEdnsVersionNonZero) {
    DNS::QueryBuilder builder;
    builder.add_question("example.com", DNS::RecordType::A);
    builder.add_edns(4096, 1, false);
    EXPECT_THROW(
        {
            [[maybe_unused]] auto _ = builder.build();
        },
        DnsPacketException);
}

TEST(QueryBuilderTest, ThrowsOnEdnsPayloadTooSmall) {
    DNS::QueryBuilder builder;
    builder.add_question("example.com", DNS::RecordType::A);
    builder.add_edns(511, 0, false);
    EXPECT_THROW(
        {
            [[maybe_unused]] auto _ = builder.build();
        },
        DnsPacketException);
}

TEST(QueryBuilderTest, AcceptsEdnsPayload512) {
    DNS::QueryBuilder builder;
    builder.add_question("example.com", DNS::RecordType::A);
    builder.add_edns(512, 0, false);
    EXPECT_NO_THROW(
        {
            [[maybe_unused]] auto _ = builder.build();
        });
}

// ===========================================================================
//  Exception message content
// ===========================================================================

TEST(QueryBuilderTest, ExceptionMessageContainsRelevantInfo) {
    DNS::QueryBuilder empty;
    try {
        [[maybe_unused]] auto _ = empty.build();
        FAIL() << "Expected DnsPacketException";
    } catch (const DnsPacketException& e) {
        EXPECT_NE(std::string_view(e.what()).find("question"), std::string_view::npos);
    }
}

TEST(QueryBuilderTest, ExceptionGetName) {
    DNS::QueryBuilder empty;
    try {
        [[maybe_unused]] auto _ = empty.build();
    } catch (const DnsPacketException& e) {
        EXPECT_EQ(e.get_name(), "DnsPacketException");
    }
}

// ===========================================================================
//  RCODE
// ===========================================================================

TEST(QueryBuilderTest, RcodeSetsHeaderBits) {
    auto packet = DNS::QueryBuilder{}
        .id(0)
        .rd(false)
        .rcode(DNS::Rcode::REFUSED)
        .add_question("example.com", DNS::RecordType::A)
        .build();

    // RCODE = 5 (REFUSED) in the low 4 bits of the flags word
    EXPECT_EQ(read_u16(packet, 2), 0x0005);
}

TEST(QueryBuilderTest, RcodeNxdomain) {
    auto packet = DNS::QueryBuilder{}
        .id(0)
        .rd(false)
        .rcode(DNS::Rcode::NXDOMAIN)
        .add_question("example.com", DNS::RecordType::A)
        .build();

    EXPECT_EQ(read_u16(packet, 2), 0x0003);
}

// ===========================================================================
//  EDNS0 — default / empty options
// ===========================================================================

TEST(QueryBuilderTest, EdnsDefaultOptions) {
    // add_edns with no options argument uses the default std::span{}
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .add_edns(4096)
        .build();

    size_t opt_offset = 29;
    // RDLENGTH should be 0 (no options)
    EXPECT_EQ(read_u16(packet, opt_offset + 9), 0);
}

TEST(QueryBuilderTest, EdnsEmptyExplicitOptions) {
    std::vector<DNS::EdnsOption> empty_opts;
    auto packet = DNS::QueryBuilder{}
        .add_question("example.com", DNS::RecordType::A)
        .add_edns(4096, 0, false, empty_opts)
        .build();

    size_t opt_offset = 29;
    EXPECT_EQ(read_u16(packet, opt_offset + 9), 0);
}
