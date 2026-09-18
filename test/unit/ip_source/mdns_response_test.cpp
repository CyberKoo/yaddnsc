#include "infrastructure/ip_source/mdns_response.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "domain/dns/record_kind.h"
#include "domain/network/inet_address.h"

namespace {
void append_u16(std::vector<std::uint8_t>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    append_u16(out, static_cast<std::uint16_t>(value >> 16));
    append_u16(out, static_cast<std::uint16_t>(value));
}

void append_name(std::vector<std::uint8_t>& out, std::string_view name) {
    size_t start = 0;
    while (start < name.size()) {
        const auto dot = name.find('.', start);
        const auto length = (dot == std::string_view::npos) ? name.size() - start : dot - start;
        out.push_back(static_cast<std::uint8_t>(length));
        out.insert(out.end(), name.begin() + static_cast<std::ptrdiff_t>(start),
                   name.begin() + static_cast<std::ptrdiff_t>(start + length));
        start = dot == std::string_view::npos ? name.size() : dot + 1;
    }
    out.push_back(0);
}

void append_answer(std::vector<std::uint8_t>& out,
                   std::string_view name,
                   const std::uint16_t type,
                   std::span<const std::uint8_t> rdata) {
    append_name(out, name);
    append_u16(out, type);
    append_u16(out, 1);
    append_u32(out, 60);
    append_u16(out, static_cast<std::uint16_t>(rdata.size()));
    out.insert(out.end(), rdata.begin(), rdata.end());
}

std::vector<std::uint8_t> response(
    std::initializer_list<std::tuple<std::string_view, std::uint16_t, std::vector<std::uint8_t>>> answers) {
    std::vector<std::uint8_t> packet;
    append_u16(packet, 0);
    append_u16(packet, 0x8400);
    append_u16(packet, 0);
    append_u16(packet, static_cast<std::uint16_t>(answers.size()));
    append_u16(packet, 0);
    append_u16(packet, 0);
    for (const auto& [name, type, rdata] : answers) {
        append_answer(packet, name, type, rdata);
    }
    return packet;
}
}  // namespace

TEST(MdnsResponse, MatchesOwnerIgnoringCaseAndTrailingDot) {
    const auto packet = response({{"Printer.LOCAL.", 1, {192, 0, 2, 7}}});
    const auto result = Mdns::parse_response(packet, "printer.local", RecordKind::A);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.front().to_string(), "192.0.2.7");
}

TEST(MdnsResponse, FiltersOwnerAndRecordType) {
    const auto packet = response({
        {"other.local", 1, {192, 0, 2, 1}},
        {"printer.local", 28, {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
        {"printer.local", 1, {198, 51, 100, 9}},
    });

    const auto ipv4 = Mdns::parse_response(packet, "printer.local.", RecordKind::A);
    ASSERT_EQ(ipv4.size(), 1);
    EXPECT_EQ(ipv4.front().to_string(), "198.51.100.9");

    const auto ipv6 = Mdns::parse_response(packet, "printer.local", RecordKind::AAAA);
    ASSERT_EQ(ipv6.size(), 1);
    EXPECT_EQ(ipv6.front().to_string(), "2001:db8::1");
}

TEST(MdnsResponse, AcceptsMultipleMatchingAnswers) {
    const auto packet = response({
        {"printer.local", 1, {192, 0, 2, 10}},
        {"printer.local", 1, {192, 0, 2, 11}},
    });
    const auto result = Mdns::parse_response(packet, "printer.local", RecordKind::A);
    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0].to_string(), "192.0.2.10");
    EXPECT_EQ(result[1].to_string(), "192.0.2.11");
}

TEST(MdnsResponse, RejectsMalformedPacketAndInvalidRdata) {
    EXPECT_THROW(
        static_cast<void>(Mdns::parse_response(std::vector<std::uint8_t>{0, 1}, "printer.local", RecordKind::A)),
        std::exception);
    const auto invalid_rdata = response({{"printer.local", 1, {192, 0, 2}}});
    EXPECT_THROW(static_cast<void>(Mdns::parse_response(invalid_rdata, "printer.local", RecordKind::A)),
                 std::exception);
}
