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
#include <vector>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "dns/validator.h"
#include "exception/dns_lookup.h"
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

} // anonymous namespace

// ===========================================================================
// Happy path
// ===========================================================================

TEST(DnsValidatorTest, ValidResponse_DoesNotThrow) {
    auto query = make_query_example(0x1234);
    auto response = make_valid_response(0x1234);

    EXPECT_NO_THROW(DNS::Validator::validate_response(query, response));
}

// ===========================================================================
// check_min_header_size
// ===========================================================================

TEST(DnsValidatorTest, ResponseTooShort_Throws) {
    auto query = make_query_example();
    std::vector<std::uint8_t> short_response(11, 0);  // less than 12 bytes

    EXPECT_THROW(
        {
            try {
                DNS::Validator::validate_response(query, short_response);
            } catch (const DnsLookupException &e) {
                EXPECT_EQ(e.get_error(), DnsError::PARSE);
                throw;
            }
        },
        DnsLookupException
    );
}

// ===========================================================================
// check_qr_bit
// ===========================================================================

TEST(DnsValidatorTest, QrBitNotSet_Throws) {
    auto query = make_query_example();
    auto response = make_query_example();  // This is a query, not a response

    EXPECT_THROW(
        {
            try {
                DNS::Validator::validate_response(query, response);
            } catch (const DnsLookupException &e) {
                EXPECT_EQ(e.get_error(), DnsError::PARSE);
                throw;
            }
        },
        DnsLookupException
    );
}

// ===========================================================================
// check_txid
// ===========================================================================

TEST(DnsValidatorTest, TxidMismatch_Throws) {
    auto query = make_query_example(0x1234);
    auto response = make_valid_response(0x5678);  // different TXID

    EXPECT_THROW(
        {
            try {
                DNS::Validator::validate_response(query, response);
            } catch (const DnsLookupException &e) {
                EXPECT_EQ(e.get_error(), DnsError::PARSE);
                throw;
            }
        },
        DnsLookupException
    );
}

// ===========================================================================
// check_qdcount
// ===========================================================================

TEST(DnsValidatorTest, QdcountNotOne_Throws) {
    auto query = make_query_example();
    auto response = make_valid_response();

    // Set QDCOUNT = 0 (not 1)
    response[4] = 0;
    response[5] = 0;

    EXPECT_THROW(
        {
            try {
                DNS::Validator::validate_response(query, response);
            } catch (const DnsLookupException &e) {
                EXPECT_EQ(e.get_error(), DnsError::PARSE);
                throw;
            }
        },
        DnsLookupException
    );
}

// ===========================================================================
// check_question_echo
// ===========================================================================

TEST(DnsValidatorTest, QuestionEchoMismatch_Throws) {
    auto query = make_query_example();
    auto response = make_valid_response();

    // Corrupt the question section in the response.
    // Change the first label length from 7 to 8.
    // DNS header is 12 bytes (per RFC 1035 §4.1.1).
    response[12] = 8;  // was 7 — now a different QNAME

    EXPECT_THROW(
        {
            try {
                DNS::Validator::validate_response(query, response);
            } catch (const DnsLookupException &e) {
                EXPECT_EQ(e.get_error(), DnsError::PARSE);
                throw;
            }
        },
        DnsLookupException
    );
}

// ===========================================================================
// check_txid — second byte mismatch (first byte matches)
// ===========================================================================

TEST(DnsValidatorTest, Txid_FirstByteMatches_SecondByteMismatch_Throws) {
    auto query = make_query_example(0x1234);
    // Response TXID: first byte matches (0x12), second byte doesn't (0xFF vs 0x34)
    auto response = make_valid_response(0x12FF);

    EXPECT_THROW(
        {
            try {
                DNS::Validator::validate_response(query, response);
            } catch (const DnsLookupException &e) {
                EXPECT_EQ(e.get_error(), DnsError::PARSE);
                EXPECT_NE(std::string_view(e.what()).find("transaction ID"),
                          std::string_view::npos);
                throw;
            }
        },
        DnsLookupException
    );
}

// ===========================================================================
// question_section_end — truncated response after QNAME but before QTYPE+QCLASS
// ===========================================================================

TEST(DnsValidatorTest, TruncatedResponse_NoRoomForQtypeQclass) {
    // Build a response where the QNAME parses successfully but the buffer
    // ends before QTYPE+QCLASS (4 bytes needed after the name).
    // A valid QNAME for "example.com" is 13 bytes (\x07example\x03com\x00).
    // The header is 12 bytes. 12 + 13 = 25. We need 25 + 4 = 29 for QTYPE+QCLASS.
    // Create a 28-byte response: 25 + 3 bytes of partial data.
    auto query = make_query_example();
    auto response = make_valid_response();

    // Truncate to drop the last byte (now 28 bytes total: header + QNAME + QTYPE + 1 byte of QCLASS)
    response.resize(28);

    EXPECT_THROW(
        {
            try {
                DNS::Validator::validate_response(query, response);
            } catch (const DnsLookupException &e) {
                EXPECT_EQ(e.get_error(), DnsError::PARSE);
                // Should be the "malformed question section" message
                EXPECT_NE(std::string_view(e.what()).find("malformed"),
                          std::string_view::npos);
                throw;
            }
        },
        DnsLookupException
    );
}

// ===========================================================================
// Multiple failures — first check wins
// ===========================================================================

TEST(DnsValidatorTest, TooShort_ReportedBeforeOtherChecks) {
    auto query = make_query_example();
    std::vector<std::uint8_t> too_short(5, 0);

    // Should fail at "too short" before checking any other field.
    EXPECT_THROW(DNS::Validator::validate_response(query, too_short), DnsLookupException);
}

// ===========================================================================
// check_question_echo — second throw path (both parseable but different)
// ===========================================================================

TEST(DnsValidatorTest, QuestionSectionEcho_ParseableButDifferent_Throws) {
    auto query = make_query_example();
    auto response = make_valid_response();

    // Change one byte inside the QNAME but keep label structure valid.
    // response[13] is the first byte of "example" (response[12]=7, response[13..19]="example")
    // Changing response[13] from 'e' to 'f' makes the name "fxample" (still 7 chars).
    response[13] = 'f';  // was 'e'

    // Both question sections are well-formed but differ → second throw path.
    EXPECT_THROW(
        {
            try {
                DNS::Validator::validate_response(query, response);
            } catch (const DnsLookupException &e) {
                EXPECT_EQ(e.get_error(), DnsError::PARSE);
                // Verify it's the "section mismatch" message, not "malformed"
                EXPECT_NE(std::string_view(e.what()).find("does not match"),
                          std::string_view::npos);
                throw;
            }
        },
        DnsLookupException
    );
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
    // Build a matching request with the standard format.
    auto request = make_query_example();

    // The overall validation will fail because the raw question section bytes
    // differ (compressed vs uncompressed). But skip_name's compression pointer
    // branch is exercised during the parsing.
    EXPECT_THROW(
        {
            try {
                DNS::Validator::validate_response(request, response);
            } catch (const DnsLookupException &e) {
                EXPECT_EQ(e.get_error(), DnsError::PARSE);
                throw;
            }
        },
        DnsLookupException
    );
}
