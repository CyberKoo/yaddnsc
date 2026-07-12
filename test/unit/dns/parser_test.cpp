//
// Unit tests for dns/parser/parser_system.h / parser_system.cpp [DEPRECATED] —
// DNS response packet parsing using the libresolv-based backend.
//
// These tests will be removed before the 1.0.0 release, together with the
// system parser backend.  The native parser (parser_native) is the default.
//
// Constructs valid DNS wire-format response packets byte-by-byte and verifies
// that RecordParser correctly extracts A, AAAA, TXT, CNAME, NS record types.
// =============================================================================

#include <vector>
#include <cstdint>
#include <array>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

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
    std::vector<std::uint8_t> make_a_response(std::uint16_t txid, std::array<std::uint8_t, 4> answer_ip,
                                              std::uint32_t ttl = 300) {
        std::vector<std::uint8_t> buf;
        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80;
        write_u16_be(buf, 4, 1);
        write_u16_be(buf, 6, 1);

        encode_name(buf, "example.com");
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0x00);
        buf.push_back(0x01);

        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(static_cast<std::uint8_t>(ttl >> 24));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 16));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 8));
        buf.push_back(static_cast<std::uint8_t>(ttl & 0xFF));
        buf.push_back(0x00);
        buf.push_back(0x04);
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
        write_u16_be(buf, 4, 1);
        write_u16_be(buf, 6, 1);

        encode_name(buf, "example.com");
        buf.push_back(0x00);
        buf.push_back(0x1C);
        buf.push_back(0x00);
        buf.push_back(0x01);

        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x1C);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(static_cast<std::uint8_t>(ttl >> 24));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 16));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 8));
        buf.push_back(static_cast<std::uint8_t>(ttl & 0xFF));
        buf.push_back(0x00);
        buf.push_back(0x10);
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
        buf.push_back(0x10);
        buf.push_back(0x00);
        buf.push_back(0x01);

        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x10);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(static_cast<std::uint8_t>(ttl >> 24));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 16));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 8));
        buf.push_back(static_cast<std::uint8_t>(ttl & 0xFF));

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
        buf.push_back(0x01);
        buf.push_back(0x00);
        buf.push_back(0x01);

        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x05);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0x2C);

        size_t rdlength_offset = buf.size();
        buf.push_back(0x00);
        buf.push_back(0x00);
        size_t rdata_start = buf.size();
        encode_name(buf, cname_target);
        uint16_t rdlength = static_cast<uint16_t>(buf.size() - rdata_start);
        buf[rdlength_offset] = static_cast<uint8_t>(rdlength >> 8);
        buf[rdlength_offset + 1] = static_cast<uint8_t>(rdlength & 0xFF);
        return buf;
    }

    /// Build a DNS response with an NS record (type 2).
    std::vector<std::uint8_t> make_ns_response(std::uint16_t txid, std::string_view ns_target,
                                                std::uint32_t ttl = 300) {
        std::vector<std::uint8_t> buf;
        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80;
        write_u16_be(buf, 4, 1);
        write_u16_be(buf, 6, 1);

        encode_name(buf, "example.com");
        buf.push_back(0x00);
        buf.push_back(0x02);
        buf.push_back(0x00);
        buf.push_back(0x01);

        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x02);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(static_cast<std::uint8_t>(ttl >> 24));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 16));
        buf.push_back(static_cast<std::uint8_t>(ttl >> 8));
        buf.push_back(static_cast<std::uint8_t>(ttl & 0xFF));

        size_t rdlength_offset = buf.size();
        buf.push_back(0x00);
        buf.push_back(0x00);
        size_t rdata_start = buf.size();
        encode_name(buf, ns_target);
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
    auto response = make_aaaa_response(0x1234, {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
    EXPECT_FALSE(parsed.records[0].empty());
}

TEST(DnsParserTest, ParseLoopbackAaaa) {
    auto response = make_aaaa_response(0x1234, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
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
// Record count
// ===========================================================================

TEST(DnsParserTest, RecordCount_ReturnsCorrectCount) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 2);

    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);

    for (int i = 0; i < 2; ++i) {
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
        buf.push_back(static_cast<std::uint8_t>(i + 1));
    }

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

// ===========================================================================
// NS record parsing (type 2)
// ===========================================================================

TEST(DnsParserTest, ParseNsRecord) {
    auto response = make_ns_response(0x1234, "ns1.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "ns1.example.com");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParseNsRecord_SubdomainTarget) {
    auto response = make_ns_response(0x5678, "dns.server.example.org");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "dns.server.example.org");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// RCODE from flags
// ===========================================================================

TEST(DnsParserTest, RcodeNxdomain) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x83;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 0);
    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    auto parsed = DNS::RecordParser::parse_strings(buf);
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NXDOMAIN);
    EXPECT_TRUE(parsed.records.empty());
}

TEST(DnsParserTest, RcodeServfail) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x82;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 0);
    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    auto parsed = DNS::RecordParser::parse_strings(buf);
    EXPECT_EQ(parsed.rcode, DNS::Rcode::SERVFAIL);
    EXPECT_TRUE(parsed.records.empty());
}

TEST(DnsParserTest, RcodeRefused) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x85;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 0);
    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    auto parsed = DNS::RecordParser::parse_strings(buf);
    EXPECT_EQ(parsed.rcode, DNS::Rcode::REFUSED);
    EXPECT_TRUE(parsed.records.empty());
}

// ===========================================================================
// Empty response and edge cases (works with both backends)
// ===========================================================================

TEST(DnsParserTest, NoRecords_EmptyResponse) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 0);
    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    auto parsed = DNS::RecordParser::parse_strings(buf);
    EXPECT_TRUE(parsed.records.empty());
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, LargeTtl) {
    auto response = make_a_response(0x1234, {192, 168, 1, 1}, 2147483647U);
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "192.168.1.1");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}
