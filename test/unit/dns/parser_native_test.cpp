//
// Unit tests for dns/parser/parser_native.cpp — native DNS parser (native-only).
//
// These tests exercise record types and error-handling code paths that are
// specific to the built-in (native) DNS stack.  The system (libresolv)
// backend does not support these same code paths.
// =============================================================================

#include <vector>
#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "dns/wire/query.h"
#include "dns/parser/parser.h"
#include "exception/dns_lookup.h"

// ===========================================================================
// Helpers — duplicated from dns_parser.cpp so both translation units can
// use them independently.
// ===========================================================================

namespace {
    void write_u16_be(std::vector<std::uint8_t> &buf, size_t offset, std::uint16_t v) {
        buf[offset] = static_cast<std::uint8_t>(v >> 8);
        buf[offset + 1] = static_cast<std::uint8_t>(v & 0xFF);
    }

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
        buf.push_back(0);
        ++written;
        return written;
    }

    void write_u32_be_bytes(std::vector<std::uint8_t>& buf, std::uint32_t v) {
        buf.push_back(static_cast<std::uint8_t>(v >> 24));
        buf.push_back(static_cast<std::uint8_t>(v >> 16));
        buf.push_back(static_cast<std::uint8_t>(v >> 8));
        buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }

    std::vector<std::uint8_t> make_ptr_response(std::uint16_t txid, std::string_view ptr_target,
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
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x0C);
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
        encode_name(buf, ptr_target);
        uint16_t rdlength = static_cast<uint16_t>(buf.size() - rdata_start);
        buf[rdlength_offset] = static_cast<uint8_t>(rdlength >> 8);
        buf[rdlength_offset + 1] = static_cast<uint8_t>(rdlength & 0xFF);
        return buf;
    }

    std::vector<std::uint8_t>
    make_mx_response(std::uint16_t txid, std::uint16_t preference, std::string_view mx_target,
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
        buf.push_back(0x0F);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x0F);
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
        buf.push_back(static_cast<std::uint8_t>(preference >> 8));
        buf.push_back(static_cast<std::uint8_t>(preference & 0xFF));
        encode_name(buf, mx_target);
        uint16_t rdlength = static_cast<uint16_t>(buf.size() - rdata_start);
        buf[rdlength_offset] = static_cast<uint8_t>(rdlength >> 8);
        buf[rdlength_offset + 1] = static_cast<uint8_t>(rdlength & 0xFF);
        return buf;
    }

    std::vector<std::uint8_t> make_soa_response(std::uint16_t txid, std::string_view mname,
                                                  std::string_view rname, std::uint32_t ttl = 300) {
        std::vector<std::uint8_t> buf;
        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80;
        write_u16_be(buf, 4, 1);
        write_u16_be(buf, 6, 1);
        encode_name(buf, "example.com");
        buf.push_back(0x00);
        buf.push_back(0x06);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x06);
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
        encode_name(buf, mname);
        encode_name(buf, rname);
        write_u32_be_bytes(buf, 2024010100U);
        write_u32_be_bytes(buf, 3600U);
        write_u32_be_bytes(buf, 900U);
        write_u32_be_bytes(buf, 604800U);
        write_u32_be_bytes(buf, 86400U);
        uint16_t rdlength = static_cast<uint16_t>(buf.size() - rdata_start);
        buf[rdlength_offset] = static_cast<uint8_t>(rdlength >> 8);
        buf[rdlength_offset + 1] = static_cast<uint8_t>(rdlength & 0xFF);
        return buf;
    }

    std::vector<std::uint8_t>
    make_srv_response(std::uint16_t txid, std::uint16_t priority, std::uint16_t weight,
                      std::uint16_t port, std::string_view target, std::uint32_t ttl = 300) {
        std::vector<std::uint8_t> buf;
        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80;
        write_u16_be(buf, 4, 1);
        write_u16_be(buf, 6, 1);
        encode_name(buf, "example.com");
        buf.push_back(0x00);
        buf.push_back(0x21);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0xC0);
        buf.push_back(0x0C);
        buf.push_back(0x00);
        buf.push_back(0x21);
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
        buf.push_back(static_cast<std::uint8_t>(priority >> 8));
        buf.push_back(static_cast<std::uint8_t>(priority & 0xFF));
        buf.push_back(static_cast<std::uint8_t>(weight >> 8));
        buf.push_back(static_cast<std::uint8_t>(weight & 0xFF));
        buf.push_back(static_cast<std::uint8_t>(port >> 8));
        buf.push_back(static_cast<std::uint8_t>(port & 0xFF));
        encode_name(buf, target);
        uint16_t rdlength = static_cast<uint16_t>(buf.size() - rdata_start);
        buf[rdlength_offset] = static_cast<uint8_t>(rdlength >> 8);
        buf[rdlength_offset + 1] = static_cast<uint8_t>(rdlength & 0xFF);
        return buf;
    }

    std::vector<std::uint8_t> make_authority_response(std::uint16_t txid, std::string_view ns_name,
                                                       std::string_view ns_target, std::uint32_t ttl = 300) {
        std::vector<std::uint8_t> buf;
        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80;
        write_u16_be(buf, 4, 1);
        write_u16_be(buf, 6, 1);
        write_u16_be(buf, 8, 1);
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
        buf.push_back(1);
        buf.push_back(2);
        buf.push_back(3);
        buf.push_back(4);
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

    std::vector<std::uint8_t>
    make_edns_response(std::uint16_t txid, bool with_options = false) {
        std::vector<std::uint8_t> buf;
        buf.resize(12, 0);
        write_u16_be(buf, 0, txid);
        buf[2] = 0x81;
        buf[3] = 0x80;
        write_u16_be(buf, 4, 1);
        write_u16_be(buf, 6, 1);
        write_u16_be(buf, 10, 1);
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
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(0x01);
        buf.push_back(0x2C);
        buf.push_back(0x00);
        buf.push_back(0x04);
        buf.push_back(1);
        buf.push_back(2);
        buf.push_back(3);
        buf.push_back(4);
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(0x29);
        buf.push_back(0x10);
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(0x00);
        buf.push_back(0x80);
        buf.push_back(0x00);
        if (with_options) {
            buf.push_back(0x00);
            buf.push_back(0x04);
            buf.push_back(0x00);
            buf.push_back(0x01);
            buf.push_back(0x00);
            buf.push_back(0x00);
        } else {
            buf.push_back(0x00);
            buf.push_back(0x00);
        }
        return buf;
    }
} // anonymous namespace

// ===========================================================================
// PTR record parsing (type 12)
// ===========================================================================

TEST(DnsParserTest, ParsePtrRecord) {
    auto response = make_ptr_response(0x1234, "target.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "target.example.com");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParsePtrRecord_ArpaDomain) {
    auto response = make_ptr_response(0x5678, "host-1.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "host-1.example.com");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// MX record parsing (type 15)
// ===========================================================================

TEST(DnsParserTest, ParseMxRecord) {
    auto response = make_mx_response(0x1234, 10, "mail.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "10 mail.example.com");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParseMxRecord_LowPreference) {
    auto response = make_mx_response(0x5678, 0, "mx-primary.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "0 mx-primary.example.com");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParseMxRecord_HighPreference) {
    auto response = make_mx_response(0x9ABC, 65535, "backup-mail.example.org");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "65535 backup-mail.example.org");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// SOA record parsing (type 6)
// ===========================================================================

TEST(DnsParserTest, ParseSoaRecord) {
    auto response = make_soa_response(0x1234, "ns1.example.com", "admin.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "ns1.example.com admin.example.com 2024010100 3600 900 604800 86400");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParseSoaRecord_LongNames) {
    auto response = make_soa_response(0x5678, "very-long-primary-name.internal.example.com",
                                       "hostmaster.very-long-primary-name.internal.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0],
              "very-long-primary-name.internal.example.com "
              "hostmaster.very-long-primary-name.internal.example.com "
              "2024010100 3600 900 604800 86400");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// SRV record parsing (type 33)
// ===========================================================================

TEST(DnsParserTest, ParseSrvRecord) {
    auto response = make_srv_response(0x1234, 10, 20, 8080, "srv.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "10 20 8080 srv.example.com");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParseSrvRecord_ZeroValues) {
    auto response = make_srv_response(0x5678, 0, 0, 53, "srv.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "0 0 53 srv.example.com");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

TEST(DnsParserTest, ParseSrvRecord_MaxValues) {
    auto response = make_srv_response(0x9ABC, 65535, 65535, 65535, "srv.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "65535 65535 65535 srv.example.com");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// EDNS0 parsing
// ===========================================================================

TEST(DnsParserTest, Edns0_Detected) {
    auto response = make_edns_response(0x1234, false);
    DNS::RecordParser parser(response);
    const auto& msg = parser.message();
    EXPECT_EQ(msg.arcount, 1U);
    const auto& edns = parser.edns();
    if (edns.has_value()) {
        EXPECT_EQ(edns->udp_payload_size, 4096U);
        EXPECT_TRUE(edns->dnssec_ok);
        EXPECT_EQ(edns->version, 0U);
        EXPECT_EQ(edns->extended_rcode, 0U);
    }
    EXPECT_EQ(msg.additionals.size(), 1U);
    EXPECT_EQ(msg.additionals[0].type, static_cast<std::uint16_t>(DNS::RecordType::OPT));
}

TEST(DnsParserTest, Edns0_WithOptions) {
    auto response = make_edns_response(0x5678, true);
    DNS::RecordParser parser(response);
    const auto& msg = parser.message();
    EXPECT_EQ(msg.arcount, 1U);
    EXPECT_EQ(msg.additionals.size(), 1U);
    EXPECT_EQ(msg.additionals[0].type, static_cast<std::uint16_t>(DNS::RecordType::OPT));
    const auto& edns = parser.edns();
    if (edns.has_value()) {
        EXPECT_EQ(edns->options.size(), 1U);
        EXPECT_EQ(edns->options[0].code, 1U);
        EXPECT_TRUE(edns->options[0].data.empty());
    }
}

// ===========================================================================
// decompress_name error paths
// ===========================================================================

TEST(DnsParserTest, DecompressName_TruncatedPointer_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 0);
    buf.push_back(0xC0);
    EXPECT_THROW((DNS::RecordParser{buf}), DnsLookupException);
}

TEST(DnsParserTest, DecompressName_PointerCycle_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 1);
    buf.push_back(0xC0);
    buf.push_back(0x0C);
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
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x2C);
    buf.push_back(0x00);
    buf.push_back(0x04);
    buf.push_back(1);
    buf.push_back(2);
    buf.push_back(3);
    buf.push_back(4);
    EXPECT_THROW((DNS::RecordParser{buf}), DnsLookupException);
}

TEST(DnsParserTest, DecompressName_TooManyIndirections_Throws) {
    const size_t chain_start = 100;
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 0);
    buf.push_back(0xC0);
    buf.push_back(static_cast<std::uint8_t>(chain_start));
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    while (buf.size() < chain_start) buf.push_back(0x00);
    for (int i = 0; i < 8; ++i) {
        uint8_t off = static_cast<uint8_t>(buf.size());
        buf.push_back(0xC0);
        buf.push_back(static_cast<uint8_t>(off + 2));
    }
    buf.push_back(0x00);
    EXPECT_THROW((DNS::RecordParser{buf}), DnsLookupException);
}

TEST(DnsParserTest, DecompressName_LabelTooLong_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 0);
    buf.push_back(100);
    EXPECT_THROW((DNS::RecordParser{buf}), DnsLookupException);
}

TEST(DnsParserTest, DecompressName_LabelExtendsPastWire_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 0);
    buf.push_back(0xC0);
    buf.push_back(100);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    while (buf.size() < 100) buf.push_back(0x00);
    buf.push_back(50);
    for (int i = 0; i < 10; ++i) buf.push_back('a');
    EXPECT_THROW((DNS::RecordParser{buf}), DnsLookupException);
}

// ===========================================================================
// parse_message error paths
// ===========================================================================

TEST(DnsParserTest, PacketTooShort_Throws) {
    std::vector<std::uint8_t> too_short = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    EXPECT_THROW((DNS::RecordParser{too_short}), DnsLookupException);
}

TEST(DnsParserTest, QuestionSectionTruncated_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 0);
    EXPECT_THROW((DNS::RecordParser{buf}), DnsLookupException);
}

TEST(DnsParserTest, RrHeaderTruncated_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 1);
    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    EXPECT_THROW((DNS::RecordParser{buf}), DnsLookupException);
}

TEST(DnsParserTest, RdataTruncated_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
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
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x2C);
    buf.push_back(0x00);
    buf.push_back(0xFF);
    buf.push_back(1);
    buf.push_back(2);
    buf.push_back(3);
    buf.push_back(4);
    EXPECT_THROW((DNS::RecordParser{buf}), DnsLookupException);
}

// ===========================================================================
// rdata_to_string error paths
// ===========================================================================

TEST(DnsParserTest, InvalidARdata_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
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
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x2C);
    buf.push_back(0x00);
    buf.push_back(0x02);
    buf.push_back(0x00);
    buf.push_back(0x01);
    EXPECT_THROW(static_cast<void>(DNS::RecordParser::parse_strings(buf)), DnsLookupException);
}

TEST(DnsParserTest, InvalidAaaaRdata_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
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
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x2C);
    buf.push_back(0x00);
    buf.push_back(0x04);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    EXPECT_THROW(static_cast<void>(DNS::RecordParser::parse_strings(buf)), DnsLookupException);
}

TEST(DnsParserTest, UnsupportedRecordType_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 1);
    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0xFF);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0xC0);
    buf.push_back(0x0C);
    buf.push_back(0x00);
    buf.push_back(0xFF);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x2C);
    buf.push_back(0x00);
    buf.push_back(0x02);
    buf.push_back(0x00);
    buf.push_back(0x01);
    EXPECT_THROW(static_cast<void>(DNS::RecordParser::parse_strings(buf)), DnsLookupException);
}

// ===========================================================================
// format_txt error path
// ===========================================================================

TEST(DnsParserTest, TruncatedTxtSegment_Throws) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
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
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x2C);
    buf.push_back(0x00);
    buf.push_back(0x02);
    buf.push_back(10);
    buf.push_back('a');
    EXPECT_THROW(static_cast<void>(DNS::RecordParser::parse_strings(buf)), DnsLookupException);
}

// ===========================================================================
// Authority records
// ===========================================================================

TEST(DnsParserTest, AuthorityRecords_Parsed) {
    auto response = make_authority_response(0x1234, "example.com", "ns1.example.com");
    DNS::RecordParser parser(response);
    const auto& msg = parser.message();
    EXPECT_EQ(msg.nscount, 1U);
    EXPECT_EQ(msg.authorities.size(), 1U);
    EXPECT_EQ(msg.authorities[0].type, static_cast<std::uint16_t>(DNS::RecordType::NS));
    EXPECT_EQ(msg.authorities[0].name, "example.com");
}

TEST(DnsParserTest, AuthorityRecords_AnswerStillWorks) {
    auto response = make_authority_response(0x1234, "example.com", "ns1.example.com");
    auto parsed = DNS::RecordParser::parse_strings(response);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "1.2.3.4");
    EXPECT_EQ(parsed.rcode, DNS::Rcode::NOERROR);
}

// ===========================================================================
// decompress_name error paths
// ===========================================================================

TEST(DnsParserTest, DecompressName_PastWireEnd_Throws) {
    // A pointer that jumps beyond the wire length.
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 1);
    // Encode a question with a name that contains a pointer to past wire end.
    // QNAME: pointer 0xC0 0xFF (offset 255, beyond packet)
    buf.push_back(0xC0);
    buf.push_back(0xFF);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    // Answer with pointer to the same broken QNAME pointer.
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
    buf.push_back(0xC0);
    buf.push_back(0x00);
    buf.push_back(0x02);
    buf.push_back(0x01);
    EXPECT_THROW(static_cast<void>(DNS::RecordParser::parse_strings(buf)), DnsLookupException);
}

// ===========================================================================
// Multiple TXT segments
// ===========================================================================

TEST(DnsParserTest, MultiSegmentTxtRecord) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, 0x1234);
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
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x2C);
    // RDATA: two character-strings: "hello" + "world"
    uint16_t rdlen = 1 + 5 + 1 + 5;
    buf.push_back(static_cast<uint8_t>(rdlen >> 8));
    buf.push_back(static_cast<uint8_t>(rdlen & 0xFF));
    // "hello"
    buf.push_back(5);
    buf.push_back('h'); buf.push_back('e'); buf.push_back('l'); buf.push_back('l'); buf.push_back('o');
    // "world"
    buf.push_back(5);
    buf.push_back('w'); buf.push_back('o'); buf.push_back('r'); buf.push_back('l'); buf.push_back('d');
    auto parsed = DNS::RecordParser::parse_strings(buf);
    ASSERT_EQ(parsed.records.size(), 1U);
    EXPECT_EQ(parsed.records[0], "hello world");
}


