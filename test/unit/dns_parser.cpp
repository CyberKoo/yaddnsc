//
// Unit tests for dns/parser/parser_system.h / parser_system.cpp — DNS response packet parsing.
//
// Constructs valid DNS wire-format response packets byte-by-byte and verifies
// that RecordParser correctly extracts A, AAAA, TXT, CNAME, and MX records.
//
// DNS header layout (RFC 1035 §4.1.1):
//   Byte 0-1:   Transaction ID
//   Byte 2-3:   Flags (QR=0x80 for response)
//   Byte 4-5:   QDCOUNT = 1
//   Byte 6-7:   ANCOUNT (number of answer records)
//   Byte 8-9:   NSCOUNT = 0
//   Byte 10-11: ARCOUNT = 0
//   Byte 12+:   Question section: QNAME (length-prefixed labels) + QTYPE(2) + QCLASS(2)
//   Then:       Answer section
// =============================================================================

#include <vector>
#include <cstdint>
#include <array>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "dns/wire/query.h"
#include "dns/parser/parser.h"
#include "exception/dns_lookup.h"

// The system parser (`parser_system.h`) may include `<arpa/nameser.h>`,
// which defines NOERROR, FORMERR, SERVFAIL, NXDOMAIN, etc. as macros
// (e.g. `#define NOERROR ns_r_noerror`).  Undefine them here to prevent
// macro expansion of our `DNS::Rcode` enumerators.
#ifdef NOERROR
#  undef NOERROR
#endif

// ===========================================================================
// Helpers for constructing DNS response packets
// ===========================================================================

namespace {
    /// We'll construct DNS response packets using manual byte layout.
    /// All values are in network byte order (big-endian).

    /// Write a 16-bit big-endian value into a buffer at the given offset.
    void write_u16_be(std::vector<std::uint8_t> &buf, size_t offset, std::uint16_t v) {
        buf[offset] = static_cast<std::uint8_t>(v >> 8);
        buf[offset + 1] = static_cast<std::uint8_t>(v & 0xFF);
    }

    /// Encode a domain name (e.g. "example.com") into DNS label format
    /// and append to the buffer. Returns the number of bytes written.
    size_t encode_name(std::vector<std::uint8_t> &buf, std::string_view name) {
        size_t written = 0;
        size_t pos = 0;
        while (pos < name.size()) {
            auto dot = name.find('.', pos);
            if (dot == std::string::npos) dot = name.size();
            auto label_len = static_cast<std::uint8_t>(dot - pos);
            buf.push_back(label_len);
            ++written;
            for (size_t i = 0; i < label_len; ++i) {
                buf.push_back(static_cast<std::uint8_t>(name[pos + i]));
                ++written;
            }
            pos = dot + 1;
        }
        buf.push_back(0); // root label
        ++written;
        return written;
    }

    /// Build a minimal DNS response with a single A record.
    /// @param txid      Transaction ID (must match the query).
    /// @param answer_ip IPv4 address bytes (4 bytes).
    /// @param ttl       TTL in seconds (default: 300).
    /// @return          Complete DNS response packet bytes.
    std::vector<std::uint8_t> make_a_response(std::uint16_t txid, std::array<std::uint8_t, 4> answer_ip,
                                              std::uint32_t ttl = 300) {
        std::vector<std::uint8_t> buf;

        // ---- Header (12 bytes) ----
        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80; // Flags: QR=1, RD=1, RA=1
        write_u16_be(buf, 4, 1); // QDCOUNT = 1
        write_u16_be(buf, 6, 1); // ANCOUNT = 1
        // NSCOUNT = 0, ARCOUNT = 0 (already zero)

        // ---- Question section: "example.com" type A ----
        encode_name(buf, "example.com");
        buf.push_back(0x00);
        buf.push_back(0x01); // QTYPE = A
        buf.push_back(0x00);
        buf.push_back(0x01); // QCLASS = IN

        // ---- Answer section: A record ----
        // Name compression pointer to "example.com" in the question section.
        // The question QNAME starts at offset 12.
        buf.push_back(0xC0);
        buf.push_back(0x0C); // compression: pointer to offset 12
        buf.push_back(0x00);
        buf.push_back(0x01); // TYPE = A
        buf.push_back(0x00);
        buf.push_back(0x01); // CLASS = IN
        // TTL (4 bytes, big-endian)
        buf.push_back(static_cast<std::uint8_t>(ttl >> 24));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 16));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 8));
        buf.push_back(static_cast<std::uint8_t>(ttl & 0xFF));
        // RDLENGTH = 4
        buf.push_back(0x00);
        buf.push_back(0x04);
        // RDATA = IPv4 address
        buf.push_back(answer_ip[0]);
        buf.push_back(answer_ip[1]);
        buf.push_back(answer_ip[2]);
        buf.push_back(answer_ip[3]);

        return buf;
    }

    /// Build a minimal DNS response with a single AAAA record.
    std::vector<std::uint8_t> make_aaaa_response(std::uint16_t txid,
                                                 std::array<std::uint8_t, 16> answer_ip,
                                                 std::uint32_t ttl = 300) {
        std::vector<std::uint8_t> buf;

        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80;
        write_u16_be(buf, 4, 1); // QDCOUNT
        write_u16_be(buf, 6, 1); // ANCOUNT

        // Question
        encode_name(buf, "example.com");
        buf.push_back(0x00);
        buf.push_back(0x1C); // QTYPE = AAAA
        buf.push_back(0x00);
        buf.push_back(0x01);

        // Answer: AAAA
        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x1C); // TYPE = AAAA
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(static_cast<std::uint8_t>(ttl >> 24));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 16));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 8));
        buf.push_back(static_cast<std::uint8_t>(ttl & 0xFF));
        buf.push_back(0x00);
        buf.push_back(0x10); // RDLENGTH = 16
        for (auto byte: answer_ip) {
            buf.push_back(byte);
        }

        return buf;
    }

    /// Build a DNS response with a single TXT record.
    std::vector<std::uint8_t>
    make_txt_response(std::uint16_t txid, std::string_view txt_value, std::uint32_t ttl = 300) {
        std::vector<std::uint8_t> buf;

        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80;
        write_u16_be(buf, 4, 1);
        write_u16_be(buf, 6, 1);

        encode_name(buf, "example.com");
        buf.push_back(0x00);
        buf.push_back(0x10); // QTYPE = TXT
        buf.push_back(0x00);
        buf.push_back(0x01);

        // Answer: TXT
        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x10); // TYPE = TXT
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(static_cast<std::uint8_t>(ttl >> 24));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 16));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 8));
        buf.push_back(static_cast<std::uint8_t>(ttl & 0xFF));

        // TXT RDATA: <character-string> = length byte + string data
        auto txt_len = static_cast<std::uint8_t>(txt_value.size());
        auto rdlength = static_cast<std::uint16_t>(1 + txt_len);
        buf.push_back(static_cast<std::uint8_t>(rdlength >> 8));
        buf.push_back(static_cast<std::uint8_t>(rdlength & 0xFF));
        buf.push_back(txt_len);
        for (auto ch: txt_value) {
            buf.push_back(static_cast<std::uint8_t>(ch));
        }

        return buf;
    }

    /// Build a DNS response with a CNAME record.
    std::vector<std::uint8_t> make_cname_response(std::uint16_t txid, std::string_view cname_target) {
        std::vector<std::uint8_t> buf;

        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80;
        write_u16_be(buf, 4, 1);
        write_u16_be(buf, 6, 1);

        encode_name(buf, "www.example.com");
        buf.push_back(0x00);
        buf.push_back(0x01); // QTYPE = A
        buf.push_back(0x00);
        buf.push_back(0x01);

        // Answer: CNAME
        buf.push_back(0xC0);
        buf.push_back(0x0C); // pointer to QNAME
        buf.push_back(0x00);
        buf.push_back(0x05); // TYPE = CNAME
        buf.push_back(0x00);
        buf.push_back(0x01); // CLASS = IN
        // TTL = 300
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0x2C);
        // RDLENGTH placeholder (will update after encoding target)
        size_t rdlength_offset = buf.size();
        buf.push_back(0x00);
        buf.push_back(0x00);
        // RDATA: compressed domain name
        size_t rdata_start = buf.size();
        encode_name(buf, cname_target);
        uint16_t rdlength = static_cast<uint16_t>(buf.size() - rdata_start);
        buf[rdlength_offset] = static_cast<uint8_t>(rdlength >> 8);
        buf[rdlength_offset + 1] = static_cast<uint8_t>(rdlength & 0xFF);

        return buf;
    }
} // anonymous namespace

// ===========================================================================
// A record parsing
// ===========================================================================

TEST(DnsParserTest, ParseSingleARecord) {
    auto response = make_a_response(0x1234, {192, 168, 1, 1});
    auto parsed = DNS::RecordParser::parse_strings(response);

    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "192.168.1.1");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParseLoopbackRecord) {
    auto response = make_a_response(0x5678, {127, 0, 0, 1});
    auto parsed = DNS::RecordParser::parse_strings(response);

    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "127.0.0.1");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParseDnsServerRecord) {
    auto response = make_a_response(0x9ABC, {8, 8, 8, 8});
    auto parsed = DNS::RecordParser::parse_strings(response);

    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "8.8.8.8");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// AAAA record parsing
// ===========================================================================

TEST(DnsParserTest, ParseSingleAaaaRecord) {
    auto response = make_aaaa_response(
        0x1234, {
            0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 1
        }
    ); // 2001:db8::1
    auto parsed = DNS::RecordParser::parse_strings(response);

    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
    // inet_ntop may format as "2001:db8::1" or "2001:db8:0:0:0:0:0:1"
    const auto &s = parsed.records[0];
    EXPECT_FALSE(s.empty());
    EXPECT_TRUE(s.find("2001") != std::string::npos);
    EXPECT_TRUE(s.find("db8") != std::string::npos || s.find("0db8") != std::string::npos);
    EXPECT_TRUE(s.find("::1") != std::string::npos || s.find(":1") != std::string::npos);
}

TEST(DnsParserTest, ParseLoopbackAaaa) {
    auto response = make_aaaa_response(
        0x1234, {
            0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 1
        }
    ); // ::1
    auto parsed = DNS::RecordParser::parse_strings(response);

    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "::1");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// TXT record parsing
// ===========================================================================

TEST(DnsParserTest, ParseTxtRecord) {
    auto response = make_txt_response(0x1234, "hello=world");
    auto parsed = DNS::RecordParser::parse_strings(response);

    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "hello=world");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParseEmptyTxtRecord) {
    auto response = make_txt_response(0x1234, "");
    auto parsed = DNS::RecordParser::parse_strings(response);

    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_TRUE(parsed.records[0].empty());
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// CNAME record parsing
// ===========================================================================

TEST(DnsParserTest, ParseCnameRecord) {
    auto response = make_cname_response(0x1234, "target.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);

    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "target.example.com");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// record_count
// ===========================================================================

TEST(DnsParserTest, RecordCount_ReturnsCorrectCount) {
    auto response = make_a_response(0x1234, {1, 2, 3, 4});
    // Add a second A record (manually append another answer)
    // For simplicity, construct a response with 2 A records
    std::vector<std::uint8_t> buf;

    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1); // QDCOUNT
    write_u16_be(buf, 6, 2); // ANCOUNT = 2

    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);

    // Answer 1: 10.0.0.1
    buf.push_back(0xC0);
    buf.push_back(0x0C);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x2C); // TTL = 300
    buf.push_back(0x00);
    buf.push_back(0x04);
    buf.push_back(10);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(1);

    // Answer 2: 10.0.0.2
    buf.push_back(0xC0);
    buf.push_back(0x0C);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x2C);
    buf.push_back(0x00);
    buf.push_back(0x04);
    buf.push_back(10);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(2);

    DNS::RecordParser parser(buf);
    EXPECT_EQ(parser.record_count(), 2U);

    auto parsed = DNS::RecordParser::parse_strings(buf);
    ASSERT_EQ(parsed.records.size(), 2U);
    EXPECT_EQ(parsed.records[0], "10.0.0.1");
    EXPECT_EQ(parsed.records[1], "10.0.0.2");
}

// ===========================================================================
// Error handling
// ===========================================================================

TEST(DnsParserTest, InvalidPacket_ThrowsDnsLookupException) {
    std::vector<std::uint8_t> garbage = {0, 1, 2, 3, 4, 5};
    EXPECT_THROW((DNS::RecordParser{garbage}), DnsLookupException);
}

TEST(DnsParserTest, EmptyBuffer_Throws) {
    EXPECT_THROW(DNS::RecordParser(std::span<const std::uint8_t>{}), DnsLookupException);
}
