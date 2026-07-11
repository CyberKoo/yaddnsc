//
// Benchmarks for DNS response packet parsing (native parser).
//
// Constructs wire-format DNS response packets for common record types
// (A, AAAA, TXT, CNAME) and measures the throughput of RecordParser.
// =============================================================================

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dns/parser/parser.h"

// =============================================================================
// Helpers — minimal DNS wire-format construction
// =============================================================================
namespace {

void write_u16_be(std::vector<std::uint8_t> &buf, std::uint16_t val) {
    buf.push_back(static_cast<std::uint8_t>(val >> 8));
    buf.push_back(static_cast<std::uint8_t>(val & 0xff));
}

size_t encode_name(std::vector<std::uint8_t> &buf, std::string_view name) {
    size_t written = 0;
    size_t pos = 0;
    while (pos < name.size()) {
        auto dot = name.find('.', pos);
        auto label_len = (dot == std::string_view::npos) ? (name.size() - pos) : (dot - pos);
        buf.push_back(static_cast<std::uint8_t>(label_len));
        for (size_t i = 0; i < label_len; ++i) {
            buf.push_back(static_cast<std::uint8_t>(name[pos + i]));
        }
        written += 1 + label_len;
        pos += label_len + (dot != std::string_view::npos ? 1 : 0);
    }
    buf.push_back(0);  // root label
    return written + 1;
}

std::vector<std::uint8_t> make_a_response(std::string_view qname) {
    std::vector<std::uint8_t> buf;
    // Header (12 bytes): ID=0x1234, QR=1, RA=1, QD=1, AN=1
    buf.push_back(0x12); buf.push_back(0x34);  // ID
    buf.push_back(0x80); buf.push_back(0x80);  // flags
    write_u16_be(buf, 1);  // QDCOUNT
    write_u16_be(buf, 1);  // ANCOUNT
    write_u16_be(buf, 0);  // NSCOUNT
    write_u16_be(buf, 0);  // ARCOUNT

    // Question section
    encode_name(buf, qname);
    write_u16_be(buf, 1);   // QTYPE A
    write_u16_be(buf, 1);   // QCLASS IN

    // Answer section: name pointer, TYPE A, CLASS IN, TTL 300, RDLENGTH 4
    buf.push_back(0xC0); buf.push_back(0x0C);  // name pointer to question
    write_u16_be(buf, 1);   // TYPE A
    write_u16_be(buf, 1);   // CLASS IN
    write_u16_be(buf, 0); write_u16_be(buf, 300);  // TTL
    write_u16_be(buf, 4);   // RDLENGTH
    buf.push_back(192); buf.push_back(168); buf.push_back(1); buf.push_back(1);

    return buf;
}

std::vector<std::uint8_t> make_aaaa_response(std::string_view qname) {
    std::vector<std::uint8_t> buf;
    buf.push_back(0x12); buf.push_back(0x34);
    buf.push_back(0x80); buf.push_back(0x80);
    write_u16_be(buf, 1); write_u16_be(buf, 1); write_u16_be(buf, 0); write_u16_be(buf, 0);

    encode_name(buf, qname);
    write_u16_be(buf, 28);  // QTYPE AAAA
    write_u16_be(buf, 1);

    buf.push_back(0xC0); buf.push_back(0x0C);
    write_u16_be(buf, 28);  // TYPE AAAA
    write_u16_be(buf, 1);
    write_u16_be(buf, 0); write_u16_be(buf, 300);
    write_u16_be(buf, 16);  // RDLENGTH — 16 bytes for IPv6
    // 2001:0db8:0000:0000:0000:0000:0000:0001
    buf.push_back(0x20); buf.push_back(0x01);  // 2001
    buf.push_back(0x0D); buf.push_back(0xB8);  // 0db8
    for (int i = 0; i < 5; ++i) {              // 0000:0000:0000:0000:0000
        buf.push_back(0); buf.push_back(0);
    }
    buf.push_back(0); buf.push_back(1);         // 0001

    return buf;
}

std::vector<std::uint8_t> make_txt_response(std::string_view qname, std::string_view text) {
    std::vector<std::uint8_t> buf;
    buf.push_back(0x12); buf.push_back(0x34);
    buf.push_back(0x80); buf.push_back(0x80);
    write_u16_be(buf, 1); write_u16_be(buf, 1); write_u16_be(buf, 0); write_u16_be(buf, 0);

    encode_name(buf, qname);
    write_u16_be(buf, 16);  // QTYPE TXT
    write_u16_be(buf, 1);

    buf.push_back(0xC0); buf.push_back(0x0C);
    write_u16_be(buf, 16);  // TYPE TXT
    write_u16_be(buf, 1);
    write_u16_be(buf, 0); write_u16_be(buf, 300);

    auto txt_len = text.size();
    write_u16_be(buf, static_cast<uint16_t>(1 + txt_len));  // RDLENGTH
    buf.push_back(static_cast<uint8_t>(txt_len));  // TXT length byte
    for (auto c : text) buf.push_back(static_cast<uint8_t>(c));

    return buf;
}

std::vector<std::uint8_t> make_cname_response(std::string_view qname, std::string_view cname) {
    std::vector<std::uint8_t> buf;
    buf.push_back(0x12); buf.push_back(0x34);
    buf.push_back(0x80); buf.push_back(0x80);
    write_u16_be(buf, 1); write_u16_be(buf, 1); write_u16_be(buf, 0); write_u16_be(buf, 0);

    encode_name(buf, qname);
    write_u16_be(buf, 5);  // QTYPE CNAME
    write_u16_be(buf, 1);

    buf.push_back(0xC0); buf.push_back(0x0C);
    write_u16_be(buf, 5);  // TYPE CNAME
    write_u16_be(buf, 1);
    write_u16_be(buf, 0); write_u16_be(buf, 300);

    // RDATA: domain name (compressed pointer to name in answer section)
    // We'll use a non-compressed name for simplicity
    size_t rdlength_offset = buf.size();
    write_u16_be(buf, 0);  // placeholder RDLENGTH
    size_t rdata_start = buf.size();
    encode_name(buf, cname);
    uint16_t rdlength = static_cast<uint16_t>(buf.size() - rdata_start);
    buf[rdlength_offset] = static_cast<uint8_t>(rdlength >> 8);
    buf[rdlength_offset + 1] = static_cast<uint8_t>(rdlength & 0xff);

    return buf;
}

}  // anonymous namespace

// =============================================================================
// Benchmarks
// =============================================================================

static void BM_DnsParseA(benchmark::State &state) {
    auto response = make_a_response("example.com");
    for (auto _ : state) {
        auto parsed = DNS::RecordParser::parse_strings(response);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_DnsParseA);

static void BM_DnsParseAAAA(benchmark::State &state) {
    auto response = make_aaaa_response("example.com");
    for (auto _ : state) {
        auto parsed = DNS::RecordParser::parse_strings(response);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_DnsParseAAAA);

static void BM_DnsParseTXT(benchmark::State &state) {
    auto response = make_txt_response("example.com", "v=spf1 include:_spf.example.com ~all");
    for (auto _ : state) {
        auto parsed = DNS::RecordParser::parse_strings(response);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_DnsParseTXT);

static void BM_DnsParseCNAME(benchmark::State &state) {
    auto response = make_cname_response("www.example.com", "www-behind-cdn.example.com");
    for (auto _ : state) {
        auto parsed = DNS::RecordParser::parse_strings(response);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_DnsParseCNAME);

static void BM_DnsParseMultiQuestion(benchmark::State &state) {
    // Build a response with 1 answer record from a single-question query.
    auto response = make_a_response("example.com");
    for (auto _ : state) {
        DNS::RecordParser parser(response);
        auto count = parser.record_count();
        benchmark::DoNotOptimize(count);
    }
}
BENCHMARK(BM_DnsParseMultiQuestion);


