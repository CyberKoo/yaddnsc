//
// Unit tests for the malformed-packet rejection paths of
// dns::RecordParser (src/infrastructure/dns/parser.cpp).
//
// The RDATA formatters are private, so every case is driven through the
// public parse_strings()/parse_message() entry points using hand-built
// packets whose answer section violates RFC 1035 length and framing rules.
// =============================================================================

#include "infrastructure/dns/parser.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/types.h"

using dns::RecordParser;
using dns::RecordType;

namespace {

void write_u16_be(std::vector<std::uint8_t>& buf, size_t offset, std::uint16_t value) {
    buf[offset] = static_cast<std::uint8_t>(value >> 8);
    buf[offset + 1] = static_cast<std::uint8_t>(value & 0xFF);
}

/// Append a 16-bit big-endian value, growing the buffer as needed.
void append_u16_be(std::vector<std::uint8_t>& buf, std::uint16_t value) {
    buf.push_back(static_cast<std::uint8_t>(value >> 8));
    buf.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

/// Append a length-prefixed label plus the root terminator: "a." -> 01 61 00
void append_label(std::vector<std::uint8_t>& buf, char ch) {
    buf.push_back(1);
    buf.push_back(static_cast<std::uint8_t>(ch));
    buf.push_back(0);
}

/// Encode a dotted name in DNS label form, terminated by the root label.
void append_name(std::vector<std::uint8_t>& buf, std::initializer_list<const char*> labels) {
    for (const auto* label : labels) {
        const size_t len = std::char_traits<char>::length(label);
        buf.push_back(static_cast<std::uint8_t>(len));
        for (size_t i = 0; i < len; ++i) {
            buf.push_back(static_cast<std::uint8_t>(label[i]));
        }
    }
    buf.push_back(0);
}

/// parse_strings()/parse_message() are [[nodiscard]]; these wrappers consume
/// the result so the EXPECT_THROW call sites stay readable.
void parse_strings_consume(const std::vector<std::uint8_t>& buf) {
    const auto parsed = RecordParser::parse_strings(buf);
    static_cast<void>(parsed);
}

/// parse_message() is private; the public constructor performs the same
/// full-message parse and throws on malformed packets.
void parse_message_consume(const std::vector<std::uint8_t>& buf) {
    const RecordParser parser(buf);
    static_cast<void>(parser.message());
}

/// Standard 12-byte header: QR + RD + RA, RCODE 0.
void append_header(std::vector<std::uint8_t>& buf, std::uint16_t qdcount, std::uint16_t ancount,
                   std::uint16_t nscount = 0, std::uint16_t arcount = 0) {
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, qdcount);
    write_u16_be(buf, 6, ancount);
    write_u16_be(buf, 8, nscount);
    write_u16_be(buf, 10, arcount);
}

/// A response with one question ("example.com" A IN) and one answer whose
/// owner name is a pointer back to that question.
///
/// `declared_rdlength` is written into the RDLENGTH field while `rdata` is
/// appended verbatim, so a test can declare a length that disagrees with the
/// bytes actually present — exactly the kind of packet a hostile peer sends.
std::vector<std::uint8_t> make_response(std::uint16_t type, std::vector<std::uint8_t> rdata,
                                        std::uint16_t declared_rdlength, std::uint16_t ttl = 300) {
    std::vector<std::uint8_t> buf(12, 0);
    append_header(buf, 1, 1);

    // Question: example.com A IN (the answer name points back at offset 12)
    append_name(buf, {"example", "com"});
    append_u16_be(buf, static_cast<std::uint16_t>(RecordType::A));
    append_u16_be(buf, 1);

    // Answer: pointer to offset 12, then the fixed RR header.
    buf.push_back(0xC0);
    buf.push_back(0x0C);
    append_u16_be(buf, type);
    append_u16_be(buf, 1);  // CLASS IN
    append_u16_be(buf, static_cast<std::uint16_t>(ttl >> 16));
    append_u16_be(buf, static_cast<std::uint16_t>(ttl & 0xFFFF));
    append_u16_be(buf, declared_rdlength);
    buf.insert(buf.end(), rdata.begin(), rdata.end());
    return buf;
}

// =============================================================================
// RDLENGTH guards in rdata_to_string
// =============================================================================

TEST(RecordParser, ParseStrings_MxRdlenBelow3_ThrowsParseError) {
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::MX), {0x00, 0x0A}, 2);
    EXPECT_THROW(parse_strings_consume(buf), DnsLookupException);
}

TEST(RecordParser, ParseStrings_SoaRdlenBelow22_ThrowsParseError) {
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::SOA), std::vector<std::uint8_t>(21, 0), 21);
    EXPECT_THROW(parse_strings_consume(buf), DnsLookupException);
}

TEST(RecordParser, ParseStrings_SrvRdlenBelow7_ThrowsParseError) {
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::SRV), std::vector<std::uint8_t>(6, 0), 6);
    EXPECT_THROW(parse_strings_consume(buf), DnsLookupException);
}

TEST(RecordParser, ParseStrings_ARdlenNot4_ThrowsParseError) {
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::A), {0x01, 0x02, 0x03}, 3);
    EXPECT_THROW(parse_strings_consume(buf), DnsLookupException);
}

TEST(RecordParser, ParseStrings_AaaaRdlenNot16_ThrowsParseError) {
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::AAAA), std::vector<std::uint8_t>(15, 0), 15);
    EXPECT_THROW(parse_strings_consume(buf), DnsLookupException);
}

// =============================================================================
// RDATA overruns (length passes the guard, name then runs past RDATA)
// =============================================================================

TEST(RecordParser, ParseStrings_MxNameExtendsPastRdata_ThrowsParseError) {
    // rdlen 3 passes the guard; the 2-byte compression pointer at the end of
    // the name position advances the cursor to rdata_offset+4.
    const std::vector<std::uint8_t> rdata{0x00, 0x0A, 0xC0, 0x0C};
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::MX), rdata, 3);
    try {
        parse_strings_consume(buf);
        FAIL() << "expected an MX RDATA boundary error";
    } catch (const DnsLookupException& error) {
        EXPECT_EQ(error.get_error(), DnsError::PARSE);
        EXPECT_STREQ(error.what(), "Invalid MX record: name extends past RDATA");
    }

    const auto valid = make_response(static_cast<std::uint16_t>(RecordType::MX), rdata, 4);
    const auto parsed = RecordParser::parse_strings(valid);
    ASSERT_EQ(parsed.records.size(), 1u);
    EXPECT_EQ(parsed.records.front(), "10 example.com");
}

TEST(RecordParser, ParseStrings_SoaNamesExtendPastRdata_ThrowsParseError) {
    // Both names decode successfully, but their 25 wire bytes exceed rdlen 22.
    std::vector<std::uint8_t> rdata;
    append_name(rdata, {"abcdefghijklmnopqrst"});
    append_name(rdata, {"b"});
    rdata.resize(45, 0);  // Five 32-bit fields follow the two names.
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::SOA), rdata, 22);
    try {
        parse_strings_consume(buf);
        FAIL() << "expected an SOA RDATA boundary error";
    } catch (const DnsLookupException& error) {
        EXPECT_EQ(error.get_error(), DnsError::PARSE);
        EXPECT_STREQ(error.what(), "Invalid SOA record: names extend past RDATA");
    }

    const auto valid = make_response(static_cast<std::uint16_t>(RecordType::SOA), rdata, 45);
    const auto parsed = RecordParser::parse_strings(valid);
    ASSERT_EQ(parsed.records.size(), 1u);
    EXPECT_EQ(parsed.records.front(), "abcdefghijklmnopqrst b 0 0 0 0 0");
}

TEST(RecordParser, ParseStrings_SoaRdataTruncatedForFixedFields_ThrowsParseError) {
    // rdlen 22, names consume 6 -> only 16 left for five 32-bit fields.
    std::vector<std::uint8_t> rdata{1, 'a', 0, 1, 'b', 0};
    rdata.resize(22, 0);
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::SOA), rdata, 22);
    EXPECT_THROW(parse_strings_consume(buf), DnsLookupException);
}

TEST(RecordParser, ParseStrings_SrvTargetExtendsPastRdata_ThrowsParseError) {
    // rdlen 7 passes the guard; the target label advances the cursor to +9.
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::SRV), {0x00, 0x01, 0x00, 0x02, 0x00,
                                                                                0x03, 0x01, 't', 0x00},
                                   7);
    EXPECT_THROW(parse_strings_consume(buf), DnsLookupException);
}

// =============================================================================
// Unsupported record types
// =============================================================================

TEST(RecordParser, ParseStrings_UnknownRecordType_ThrowsParseError) {
    const auto buf = make_response(999, {0x01, 0x02}, 2);
    EXPECT_THROW(parse_strings_consume(buf), DnsLookupException);
}

TEST(RecordParser, ParseStrings_PrivateUseType_ThrowsParseError) {
    // Unknown types (including the private-use range) are rejected outright:
    // rdata_to_string has no generic fallback, so format_generic is not
    // reachable through the public parsers.
    const auto buf = make_response(65280, {0xDE, 0xAD, 0xBE, 0xEF}, 4);
    EXPECT_THROW(parse_strings_consume(buf), DnsLookupException);
}

// =============================================================================
// Name decompression limits
// =============================================================================

TEST(RecordParser, ParseMessage_ExpandedNameOver255Bytes_ThrowsParseError) {
    // Five 63-byte labels decode to 315 characters, past the RFC 1035 §2.3.4 cap.
    std::vector<std::uint8_t> buf(12, 0);
    append_header(buf, 1, 0);
    for (int i = 0; i < 5; ++i) {
        buf.push_back(63);
        for (int j = 0; j < 63; ++j) {
            buf.push_back(static_cast<std::uint8_t>('a' + i));
        }
    }
    buf.push_back(0);
    append_u16_be(buf, static_cast<std::uint16_t>(RecordType::A));
    append_u16_be(buf, 1);

    EXPECT_THROW(parse_message_consume(buf), DnsLookupException);
}

TEST(RecordParser, ParseMessage_TruncatedRrHeader_ThrowsParseError) {
    std::vector<std::uint8_t> buf(12, 0);
    append_header(buf, 1, 1);
    append_label(buf, 'q');
    append_u16_be(buf, static_cast<std::uint16_t>(RecordType::A));
    append_u16_be(buf, 1);
    // Answer owner name, then nothing: the 10-octet RR header is absent.
    append_label(buf, 'a');

    EXPECT_THROW(parse_message_consume(buf), DnsLookupException);
}

TEST(RecordParser, ParseMessage_TruncatedRrBody_ThrowsParseError) {
    std::vector<std::uint8_t> buf(12, 0);
    append_header(buf, 1, 1);
    append_label(buf, 'q');
    append_u16_be(buf, static_cast<std::uint16_t>(RecordType::A));
    append_u16_be(buf, 1);
    // Owner name plus TYPE and CLASS, but no TTL / RDLENGTH / RDATA.
    append_label(buf, 'a');
    append_u16_be(buf, static_cast<std::uint16_t>(RecordType::A));
    append_u16_be(buf, 1);

    EXPECT_THROW(parse_message_consume(buf), DnsLookupException);
}

// =============================================================================
// Well-formed packets still parse (guard against over-strict validation)
// =============================================================================

TEST(RecordParser, ParseStrings_ValidMx_IsFormatted) {
    // preference 10 + a root-terminated name
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::MX), {0x00, 0x0A, 0x00}, 3);
    const auto parsed = RecordParser::parse_strings(buf);
    ASSERT_EQ(parsed.records.size(), 1u);
    EXPECT_EQ(parsed.records.front(), "10 ");
}

TEST(RecordParser, ParseStrings_ValidSrv_IsFormatted) {
    // priority 1, weight 2, port 3, target "t."
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::SRV), {0x00, 0x01, 0x00, 0x02, 0x00,
                                                                                0x03, 0x01, 't', 0x00},
                                   9);
    const auto parsed = RecordParser::parse_strings(buf);
    ASSERT_EQ(parsed.records.size(), 1u);
    EXPECT_EQ(parsed.records.front(), "1 2 3 t");
}

TEST(RecordParser, ParseStrings_ValidSoa_IsFormatted) {
    std::vector<std::uint8_t> rdata{1, 'a', 0, 1, 'b', 0};
    rdata.resize(26, 0);
    const auto buf = make_response(static_cast<std::uint16_t>(RecordType::SOA), rdata, 26);
    const auto parsed = RecordParser::parse_strings(buf);
    ASSERT_EQ(parsed.records.size(), 1u);
    EXPECT_EQ(parsed.records.front(), "a b 0 0 0 0 0");
}

}  // namespace
