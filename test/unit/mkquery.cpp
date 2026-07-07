//
// Unit tests for dns/proto/mkquery.h / mkquery.cpp — DNS query packet construction.
//
// Tests:
//   - mkquery_manual constructs a valid RFC 1035 query packet.
//   - mkquery_mdns constructs a valid RFC 6762 mDNS query packet.
//   - Packet field verification at known byte offsets.
// =============================================================================

#include <vector>
#include <cstdint>

#include <arpa/nameser.h>

#include <gtest/gtest.h>

#include "dns/proto/mkquery.h"
#include "address_family.h"

// ===========================================================================
// Helper: verify the DNS header of any query packet (12 bytes).
// ===========================================================================

namespace {

    /// Verify the DNS header structure for a standard query (mkquery_manual).
    void expect_standard_query_header(const std::vector<std::uint8_t> &packet, bool has_random_txid) {
        ASSERT_GE(packet.size(), 12U) << "Packet must have at least a 12-byte header";

        if (has_random_txid) {
            // Transaction ID is random — we can only verify it's non-zero
            // (extremely unlikely to be zero with a proper random device)
            EXPECT_NE(packet[0], 0) << "TXID high byte should not be 0";
        }

        // Flags: bytes 2-3 = 0x0100 (standard query, RD=1)
        EXPECT_EQ(packet[2], 0x01) << "Flags high byte: standard query with RD";
        EXPECT_EQ(packet[3], 0x00) << "Flags low byte";

        // QDCOUNT = 1 (bytes 4-5)
        EXPECT_EQ(packet[4], 0x00);
        EXPECT_EQ(packet[5], 0x01);

        // ANCOUNT = 0 (bytes 6-7)
        EXPECT_EQ(packet[6], 0x00);
        EXPECT_EQ(packet[7], 0x00);

        // NSCOUNT = 0 (bytes 8-9)
        EXPECT_EQ(packet[8], 0x00);
        EXPECT_EQ(packet[9], 0x00);

        // ARCOUNT = 0 (bytes 10-11)
        EXPECT_EQ(packet[10], 0x00);
        EXPECT_EQ(packet[11], 0x00);
    }

    /// Verify the DNS header for an mDNS query (txid=0, flags=0x0000).
    void expect_mdns_query_header(const std::vector<std::uint8_t> &packet) {
        ASSERT_GE(packet.size(), 12U);

        // TXID = 0 (RFC 6762 §18.1)
        EXPECT_EQ(packet[0], 0x00);
        EXPECT_EQ(packet[1], 0x00);

        // Flags = 0x0000 (standard query, no RD — RFC 6762 §18.1)
        EXPECT_EQ(packet[2], 0x00);
        EXPECT_EQ(packet[3], 0x00);

        // QDCOUNT = 1
        EXPECT_EQ(packet[4], 0x00);
        EXPECT_EQ(packet[5], 0x01);

        // ANCOUNT = NSCOUNT = ARCOUNT = 0
        EXPECT_EQ(packet[6], 0x00);
        EXPECT_EQ(packet[7], 0x00);
        EXPECT_EQ(packet[8], 0x00);
        EXPECT_EQ(packet[9], 0x00);
        EXPECT_EQ(packet[10], 0x00);
        EXPECT_EQ(packet[11], 0x00);
    }

    /// Verify that the QNAME at offset 12 encodes "example.com" correctly.
    void expect_qname_example_com(const std::vector<std::uint8_t> &packet, size_t offset = 12) {
        // \x07example\x03com\x00
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
        EXPECT_EQ(packet[offset + 12], 0);  // root label
    }

    /// Verify that a single-label QNAME encodes "local" correctly.
    void expect_qname_local(const std::vector<std::uint8_t> &packet, size_t offset = 12) {
        // \x05local\x00
        ASSERT_GE(packet.size(), offset + 7);
        EXPECT_EQ(packet[offset + 0], 5);
        EXPECT_EQ(packet[offset + 1], 'l');
        EXPECT_EQ(packet[offset + 2], 'o');
        EXPECT_EQ(packet[offset + 3], 'c');
        EXPECT_EQ(packet[offset + 4], 'a');
        EXPECT_EQ(packet[offset + 5], 'l');
        EXPECT_EQ(packet[offset + 6], 0);
    }

    /// Compute QNAME length from the encoded form (sum of label lengths + labels + root).
    size_t encoded_qname_length(const std::vector<std::uint8_t> &packet, size_t offset = 12) {
        size_t len = 0;
        while (offset + len < packet.size()) {
            auto label_len = packet[offset + len];
            ++len;  // length byte
            if (label_len == 0) break;  // root label
            len += label_len;
        }
        return len;
    }

} // anonymous namespace

// ===========================================================================
// mkquery_manual — standard DNS query
// ===========================================================================

TEST(MkqueryManualTest, BuildsExampleCom_A) {
    auto packet = DNS::mkquery_manual("example.com", ns_t_a);

    expect_standard_query_header(packet, true);
    expect_qname_example_com(packet, 12);

    // QTYPE (after QNAME end): ns_t_a = 1 → 0x0001
    size_t qname_len = encoded_qname_length(packet, 12);
    size_t qtype_offset = 12 + qname_len;
    ASSERT_GE(packet.size(), qtype_offset + 4);
    EXPECT_EQ(packet[qtype_offset], 0x00);
    EXPECT_EQ(packet[qtype_offset + 1], 0x01);  // A record

    // QCLASS = IN (1) → 0x0001
    EXPECT_EQ(packet[qtype_offset + 2], 0x00);
    EXPECT_EQ(packet[qtype_offset + 3], 0x01);
}

TEST(MkqueryManualTest, BuildsGoogleCom_AAAA) {
    auto packet = DNS::mkquery_manual("google.com", ns_t_aaaa);

    expect_standard_query_header(packet, true);

    // QNAME: \x06google\x03com\x00
    ASSERT_GE(packet.size(), 12 + 11);
    EXPECT_EQ(packet[12], 6);
    EXPECT_EQ(packet[13], 'g');
    EXPECT_EQ(packet[14], 'o');
    EXPECT_EQ(packet[15], 'o');
    EXPECT_EQ(packet[16], 'g');
    EXPECT_EQ(packet[17], 'l');
    EXPECT_EQ(packet[18], 'e');
    EXPECT_EQ(packet[19], 3);
    EXPECT_EQ(packet[20], 'c');
    EXPECT_EQ(packet[21], 'o');
    EXPECT_EQ(packet[22], 'm');
    EXPECT_EQ(packet[23], 0);

    // QTYPE = AAAA (28) → 0x001C
    EXPECT_EQ(packet[24], 0x00);
    EXPECT_EQ(packet[25], 0x1C);

    // QCLASS = IN (1)
    EXPECT_EQ(packet[26], 0x00);
    EXPECT_EQ(packet[27], 0x01);
}

TEST(MkqueryManualTest, TotalPacketSize) {
    auto packet = DNS::mkquery_manual("example.com", ns_t_a);
    // header(12) + QNAME(\x07example\x03com\x00 = 13) + QTYPE(2) + QCLASS(2) = 29
    EXPECT_EQ(packet.size(), 29U);
}

TEST(MkqueryManualTest, BuildsDeepSubdomain) {
    auto packet = DNS::mkquery_manual("a.b.c.example.com", ns_t_a);

    expect_standard_query_header(packet, true);

    // QNAME: \x01a\x01b\x01c\x07example\x03com\x00 = 1+1+1+7+3+1 = 14 bytes content + 6 label bytes
    // Actually: \x01 a \x01 b \x01 c \x07 e x a m p l e \x03 c o m \x00
    // Labels: 1, 1, 1, 7, 3, 0 → total 1+1+1+1+1+7+1+3+1+0 = 17 bytes of QNAME
    ASSERT_GE(packet.size(), 12 + 17 + 4);
    EXPECT_EQ(packet[12], 1);
    EXPECT_EQ(packet[13], 'a');
    EXPECT_EQ(packet[14], 1);
    EXPECT_EQ(packet[15], 'b');
    EXPECT_EQ(packet[16], 1);
    EXPECT_EQ(packet[17], 'c');
    EXPECT_EQ(packet[18], 7);
    EXPECT_EQ(packet[19], 'e');
    EXPECT_EQ(packet[20], 'x');
    EXPECT_EQ(packet[21], 'a');
    EXPECT_EQ(packet[22], 'm');
    EXPECT_EQ(packet[23], 'p');
    EXPECT_EQ(packet[24], 'l');
    EXPECT_EQ(packet[25], 'e');
    EXPECT_EQ(packet[26], 3);
    EXPECT_EQ(packet[27], 'c');
    EXPECT_EQ(packet[28], 'o');
    EXPECT_EQ(packet[29], 'm');
    EXPECT_EQ(packet[30], 0);

    // QTYPE = A
    EXPECT_EQ(packet[31], 0x00);
    EXPECT_EQ(packet[32], 0x01);
    // QCLASS = IN
    EXPECT_EQ(packet[33], 0x00);
    EXPECT_EQ(packet[34], 0x01);
}

// ===========================================================================
// mkquery_mdns — mDNS query (RFC 6762)
// ===========================================================================

TEST(MkqueryMdnsTest, BasicQuery) {
    auto packet = DNS::mkquery_mdns("my-printer.local", ns_t_a);

    expect_mdns_query_header(packet);

    // QNAME: \x0Amy-printer\x05local\x00 = 10 + 5 + 1 = 16 bytes
    ASSERT_GE(packet.size(), 12 + 16 + 4);
    EXPECT_EQ(packet[12], 10);
    EXPECT_EQ(packet[13], 'm');
    EXPECT_EQ(packet[14], 'y');
    EXPECT_EQ(packet[15], '-');
    EXPECT_EQ(packet[16], 'p');
    EXPECT_EQ(packet[17], 'r');
    EXPECT_EQ(packet[18], 'i');
    EXPECT_EQ(packet[19], 'n');
    EXPECT_EQ(packet[20], 't');
    EXPECT_EQ(packet[21], 'e');
    EXPECT_EQ(packet[22], 'r');
    EXPECT_EQ(packet[23], 5);
    EXPECT_EQ(packet[24], 'l');
    EXPECT_EQ(packet[25], 'o');
    EXPECT_EQ(packet[26], 'c');
    EXPECT_EQ(packet[27], 'a');
    EXPECT_EQ(packet[28], 'l');
    EXPECT_EQ(packet[29], 0);

    // QTYPE = A (0x0001)
    EXPECT_EQ(packet[30], 0x00);
    EXPECT_EQ(packet[31], 0x01);

    // QCLASS = IN | QU = 0x8001 (unicast-response bit set)
    EXPECT_EQ(packet[32], 0x80);
    EXPECT_EQ(packet[33], 0x01);
}

TEST(MkqueryMdnsTest, UnicastResponseFalse) {
    // When unicast_rsp=false, the QU bit should NOT be set.
    // Use "local" (single label) so expect_qname_local can verify it.
    auto packet = DNS::mkquery_mdns("local", ns_t_aaaa, false);

    expect_mdns_query_header(packet);
    expect_qname_local(packet, 12);

    size_t qname_len = encoded_qname_length(packet, 12);
    size_t qclass_offset = 12 + qname_len + 2;  // QTYPE is after QNAME

    // QCLASS = IN only (0x0001), no QU bit
    EXPECT_EQ(packet[qclass_offset], 0x00);
    EXPECT_EQ(packet[qclass_offset + 1], 0x01);
}

TEST(MkqueryMdnsTest, UnicastResponseTrue) {
    // When unicast_rsp=true, the QU bit should be set.
    // Use "local" (single label) so expect_qname_local can verify it.
    auto packet = DNS::mkquery_mdns("local", ns_t_aaaa, true);

    expect_mdns_query_header(packet);
    expect_qname_local(packet, 12);

    size_t qname_len = encoded_qname_length(packet, 12);
    size_t qclass_offset = 12 + qname_len + 2;

    // QCLASS = IN | QU = 0x8001
    EXPECT_EQ(packet[qclass_offset], 0x80);
    EXPECT_EQ(packet[qclass_offset + 1], 0x01);
}

TEST(MkqueryMdnsTest, AaaaRecordType) {
    auto packet = DNS::mkquery_mdns("host.local", ns_t_aaaa);

    size_t qname_len = encoded_qname_length(packet, 12);
    size_t qtype_offset = 12 + qname_len;

    // QTYPE = AAAA (28) → 0x001C
    EXPECT_EQ(packet[qtype_offset], 0x00);
    EXPECT_EQ(packet[qtype_offset + 1], 0x1C);
}

// ===========================================================================
// mkquery — compile-time dispatch wrapper
// ===========================================================================

TEST(MkqueryTest, DispatchReturnsNonEmpty) {
    // mkquery() dispatches to mkquery_manual or mkquery_system based on
    // YADDNSC_USE_NATIVE_DNS. Both should return a non-empty packet.
    auto packet = DNS::mkquery("example.com", ns_t_a);
    EXPECT_GT(packet.size(), 12U);
}
