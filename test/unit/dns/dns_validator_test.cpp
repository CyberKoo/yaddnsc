//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for dns/validator.h / validator.cpp — DNS response validation.
//
// This test constructs minimal raw DNS wire-format queries and responses
// byte-by-byte to verify each validation check in isolation.
//
// DNS header layout (RFC 1035 §4.1.1):
//   Byte 0-1:  Transaction ID
//   Byte 2-3:  Flags (QR=0x80 for response)
//   Byte 4-5:  QDCOUNT (number of questions)
//   Byte 6-7:  ANCOUNT
//   Byte 8-9:  NSCOUNT
//   Byte 10-11: ARCOUNT
//   Byte 12+:   Question section (QNAME + QTYPE + QCLASS)
//
// A minimal question section for "example.com":
//   - QNAME:  7 e x a m p l e 3 c o m 0  (length-prefixed labels + root)
//            \x07example\x03com\x00
//   - QTYPE:  2 bytes (1 = A)
//   - QCLASS: 2 bytes (1 = IN)
// =============================================================================

#include <array>
#include <expected>
#include <vector>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "dns/validator.h"
#include "dns_error.h"

// ===========================================================================
// Test helpers — build minimal DNS wire-format messages
// ===========================================================================

namespace {

    /// Build a minimal DNS query for "example.com" type A.
    /// Returns 16 bytes: 12-byte header + 4-byte question.
    std::array<std::uint8_t, 16> make_query(std::uint16_t txid = 0x1234) {
        std::array<std::uint8_t, 16> buf{};

        // Transaction ID
        buf[0] = static_cast<std::uint8_t>(txid >> 8);
        buf[1] = static_cast<std::uint8_t>(txid & 0xFF);

        // Flags: standard query (0x0100 = recursive desired)
        buf[2] = 0x01;
        buf[3] = 0x00;

        // QDCOUNT = 1
        buf[4] = 0x00;
        buf[5] = 0x01;

        // Question: \x07example\x03com\x00
        buf[12] = 7;
        buf[13] = 'e';
        buf[14] = 'x';
        buf[15] = 'a';
        // ... This gets complex. Let's use a simpler approach below.
        return buf;
    }

    /// Build a proper minimal query with \x07example\x03com\x00 QNAME.
    /// Total: 12 (header) + 13 (QNAME) + 4 (QTYPE+QCLASS) = 29 bytes.
    std::vector<std::uint8_t> make_query_example(std::uint16_t txid = 0x1234) {
        std::vector<std::uint8_t> buf(29, 0);

        // Header
        buf[0] = static_cast<std::uint8_t>(txid >> 8);
        buf[1] = static_cast<std::uint8_t>(txid & 0xFF);
        buf[2] = 0x01;  // flags: recursive query
        buf[3] = 0x00;
        buf[4] = 0x00;  // QDCOUNT high
        buf[5] = 0x01;  // QDCOUNT low  = 1
        // ANCOUNT, NSCOUNT, ARCOUNT remain 0

        // Question: "example.com" in wire format
        // \x07example\x03com\x00
        buf[12] = 7;
        buf[13] = 'e';
        buf[14] = 'x';
        buf[15] = 'a';
        buf[16] = 'm';
        buf[17] = 'p';
        buf[18] = 'l';
        buf[19] = 'e';
        buf[20] = 3;
        buf[21] = 'c';
        buf[22] = 'o';
        buf[23] = 'm';
        buf[24] = 0;  // root label

        // QTYPE = 1 (A record)
        buf[25] = 0x00;
        buf[26] = 0x01;

        // QCLASS = 1 (IN)
        buf[27] = 0x00;
        buf[28] = 0x01;

        return buf;
    }

    /// Build a valid response to the query above.
    /// Starts with QR bit set, same TXID, echoes the question.
    std::vector<std::uint8_t> make_valid_response(std::uint16_t txid = 0x1234) {
        auto buf = make_query_example(txid);

        // Set QR bit (0x80) in flags — first response byte.
        // Original was 0x01; QR | 0x01 = 0x81
        buf[2] = 0x81;  // QR=1, opcode=0, AA=0, TC=0, RD=1
        buf[3] = 0x80;  // RA=1

        return buf;
    }

    /// Helper: assert that validate_response succeeds.
    void expect_valid(std::span<const std::uint8_t> query,
                      std::span<const std::uint8_t> response) {
        auto result = DNS::Validator::validate_response(query, response);
        EXPECT_TRUE(result.has_value()) << "expected valid, got: "
            << (result.has_value() ? "" : result.error().message);
    }

    /// Helper: assert that validate_response fails with PARSE error.
    void expect_parse_error(std::span<const std::uint8_t> query,
                            std::span<const std::uint8_t> response) {
        auto result = DNS::Validator::validate_response(query, response);
        EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, DnsError::PARSE);
        }
    }

    /// Helper: assert that validate_response fails with PARSE error
    /// and error message contains the given substring.
    void expect_parse_error_msg(std::span<const std::uint8_t> query,
                                std::span<const std::uint8_t> response,
                                std::string_view expected_substr) {
        auto result = DNS::Validator::validate_response(query, response);
        EXPECT_FALSE(result.has_value());
        if (!result.has_value()) {
            EXPECT_EQ(result.error().code, DnsError::PARSE);
            EXPECT_NE(result.error().message.find(expected_substr),
                      std::string_view::npos);
        }
    }

} // anonymous namespace

// ===========================================================================
// Happy path
// ===========================================================================

TEST(DnsValidatorTest, ValidResponse_Succeeds) {
    auto query = make_query_example(0x1234);
    auto response = make_valid_response(0x1234);

    expect_valid(query, response);
}

// ===========================================================================
// check_min_header_size
// ===========================================================================

TEST(DnsValidatorTest, ResponseTooShort_ReturnsParseError) {
    auto query = make_query_example();
    std::vector<std::uint8_t> short_response(11, 0);  // less than 12 bytes

    expect_parse_error(query, short_response);
}

// ===========================================================================
// check_qr_bit
// ===========================================================================

TEST(DnsValidatorTest, QrBitNotSet_ReturnsParseError) {
    auto query = make_query_example();
    auto response = make_query_example();  // This is a query, not a response

    expect_parse_error(query, response);
}

// ===========================================================================
// check_txid
// ===========================================================================

TEST(DnsValidatorTest, TxidMismatch_ReturnsParseError) {
    auto query = make_query_example(0x1234);
    auto response = make_valid_response(0x5678);  // different TXID

    expect_parse_error(query, response);
}

// ===========================================================================
// check_qdcount
// ===========================================================================

TEST(DnsValidatorTest, QdcountNotOne_ReturnsParseError) {
    auto query = make_query_example();
    auto response = make_valid_response();

    // Set QDCOUNT = 0 (not 1)
    response[4] = 0;
    response[5] = 0;

    expect_parse_error(query, response);
}

// ===========================================================================
// check_question_echo
// ===========================================================================

TEST(DnsValidatorTest, QuestionEchoMismatch_ReturnsParseError) {
    auto query = make_query_example();
    auto response = make_valid_response();

    // Corrupt the question section in the response.
    // Change the first label length from 7 to 8.
    // DNS header is 12 bytes (per RFC 1035 §4.1.1).
    response[12] = 8;  // was 7 — now a different QNAME

    expect_parse_error(query, response);
}

// ===========================================================================
// check_txid — second byte mismatch (first byte matches)
// ===========================================================================

TEST(DnsValidatorTest, Txid_FirstByteMatches_SecondByteMismatch_ReturnsParseError) {
    auto query = make_query_example(0x1234);
    // Response TXID: first byte matches (0x12), second byte doesn't (0xFF vs 0x34)
    auto response = make_valid_response(0x12FF);

    expect_parse_error_msg(query, response, "transaction ID");
}

// ===========================================================================
// question_section_end — truncated response after QNAME but before QTYPE+QCLASS
// ===========================================================================

TEST(DnsValidatorTest, TruncatedResponse_NoRoomForQtypeQclass) {
    auto query = make_query_example();
    auto response = make_valid_response();

    // Truncate to drop the last byte (now 28 bytes total: header + QNAME + QTYPE + 1 byte of QCLASS)
    response.resize(28);

    expect_parse_error_msg(query, response, "malformed");
}

// ===========================================================================
// Multiple failures — first check wins
// ===========================================================================

TEST(DnsValidatorTest, TooShort_ReportedBeforeOtherChecks) {
    auto query = make_query_example();
    std::vector<std::uint8_t> too_short(5, 0);

    // Should fail at "too short" before checking any other field.
    expect_parse_error(query, too_short);
}

// ===========================================================================
// question_section_end — short query (< 12 bytes) on request
// ===========================================================================

TEST(DnsValidatorTest, ShortQuery_QuestionSectionEnd_ReturnsZero) {
    std::vector<std::uint8_t> short_query{0x12, 0x34, 0x00};  // 3 bytes, TXID matches response
    auto response = make_valid_response();

    expect_parse_error(short_query, response);
}

// ===========================================================================
// question_section_end — response QDCOUNT == 0
// ===========================================================================

TEST(DnsValidatorTest, ResponseQdcountZero_QuestionSectionEnd_ReturnsZero) {
    std::vector<std::uint8_t> query = make_query_example();
    // Set QDCOUNT = 0 on the query
    query[4] = 0;
    query[5] = 0;

    auto response = make_valid_response();

    expect_parse_error_msg(query, response, "malformed");
}

// ===========================================================================
// check_question_echo — rsp_qs_end == 0 (skip_name returns 0)
// ===========================================================================

TEST(DnsValidatorTest, ResponseMalformedQname_SkipName_ReturnsZero) {
    auto query = make_query_example();
    auto response = make_valid_response();

    // Make the first label length 100 (way beyond the buffer)
    response[12] = 100;

    expect_parse_error_msg(query, response, "malformed");
}

// ===========================================================================
// check_question_echo — second throw path (both parseable but different)
// ===========================================================================

TEST(DnsValidatorTest, QuestionSectionEcho_ParseableButDifferent_ReturnsParseError) {
    auto query = make_query_example();
    auto response = make_valid_response();

    // Change one byte inside the QNAME but keep label structure valid.
    response[13] = 'f';  // was 'e'

    expect_parse_error_msg(query, response, "does not match");
}

// ===========================================================================
// skip_name compression pointer (line 33)
// ===========================================================================

TEST(DnsValidatorTest, QuestionSection_CompressionPointer_Skipped) {
    // Build a response where the question section uses a compression pointer.
    // The full QNAME is placed at a different position, and position 12 has
    // a compression pointer (0xC0, offset) to it.
    //
    // Layout:
    //   0-11:  DNS header (standard query header with QR bit)
    //   12-13: Compression pointer 0xC0 0x1C → points to byte 28
    //   14-17: QTYPE + QCLASS (4 bytes)
    //   18-27: (padding / unused)
    //   28-40: Full QNAME: \x07example\x03com\x00 (13 bytes)
    //   41-44: QTYPE + QCLASS (4 bytes) — for the expanded name
    //
    // This creates a 45-byte response.
    std::vector<std::uint8_t> response(45, 0);

    // Header: TXID 0x1234
    response[0] = 0x12;
    response[1] = 0x34;
    // Flags: QR=1, RD=1, RA=1
    response[2] = 0x81;
    response[3] = 0x80;
    // QDCOUNT = 1
    response[4] = 0x00;
    response[5] = 0x01;

    // Question: compression pointer at position 12 → points to position 28
    response[12] = 0xC0;
    response[13] = 28;

    // QTYPE = A (1) at position 14
    response[14] = 0x00;
    response[15] = 0x01;
    // QCLASS = IN (1) at position 16
    response[16] = 0x00;
    response[17] = 0x01;

    // Full QNAME at position 28
    response[28] = 7;
    response[29] = 'e';
    response[30] = 'x';
    response[31] = 'a';
    response[32] = 'm';
    response[33] = 'p';
    response[34] = 'l';
    response[35] = 'e';
    response[36] = 3;
    response[37] = 'c';
    response[38] = 'o';
    response[39] = 'm';
    response[40] = 0;

    // The request must have the same QNAME uncompressed.
    auto request = make_query_example();

    // The overall validation will fail because the raw question section bytes
    // differ (compressed vs uncompressed). But skip_name's compression pointer
    // branch is exercised during the parsing.
    expect_parse_error(request, response);
}
