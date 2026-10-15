//
// Unit tests for the bootstrap resolver's remaining paths
// (src/infrastructure/dns/bootstrap/bootstrap.cpp): the server-entry policy,
// the host/family checks that run before any packet, the handling of one
// server's response, the race that replaces a failed candidate, and the
// precedence between the two address families.
//
// bootstrap calls the classic wire exchange directly and exposes no transport
// seam, so this file supplies an in-memory replacement for
// dns::detail::query_classic. That replacement is what keeps every case below
// socket-free: it answers from the script the case installs and logs every
// call, so a lookup that tried to reach the network would fail its own
// assertions instead of opening a socket. It also means this file must be built
// into a binary of its own — no other object in that link may define
// query_classic (see the CMake wiring in the change report).
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "coro/coro.h"
#include "domain/config/dns_config.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/parser.h"
#include "infrastructure/dns/policy.h"
#include "infrastructure/dns/types.h"

namespace {

using ResolveResult = std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>;

using dns::Rcode;
using dns::RecordType;

/// One answer record of a scripted response.
struct AnswerRecord {
    RecordType type{RecordType::A};
    std::vector<std::uint8_t> rdata{};
};

/// What the in-memory exchange answers with. Exactly one of `failure`, `raw`
/// and the (rcode, answers) pair is meant to be set.
struct ScriptedReply {
    /// A transport-level failure, as the exchange reports timeouts and refused
    /// sockets.
    std::optional<domain::DnsErrorInfo> failure{};
    /// A response returned verbatim, for bytes the builder cannot produce.
    std::vector<std::uint8_t> raw{};
    /// Otherwise: the RCODE and answers of a response built around the query.
    Rcode rcode{Rcode::NOERROR};
    std::vector<AnswerRecord> answers{};
};

/// One exchange call, recorded so a case can assert which server was asked.
struct ExchangeCall {
    std::string server;
    std::uint16_t qtype;
};

/// The per-case script, keyed by server address (a specific entry in the list)
/// or by query type (every server, for the concurrent family lookups).
struct WireScript {
    std::map<std::string, ScriptedReply> by_address;
    std::map<std::uint16_t, ScriptedReply> by_type;
};

[[nodiscard]] WireScript& script() {
    static WireScript installed;
    return installed;
}

[[nodiscard]] std::vector<ExchangeCall>& calls() {
    static std::vector<ExchangeCall> log;
    return log;
}

/// Install the replies of one case; a case that builds its script from scratch
/// needs no ordering guarantee under --gtest_shuffle.
class Wire {
public:
    Wire() {
        script().by_address.clear();
        script().by_type.clear();
        calls().clear();
    }

    /// Every server asked for `qtype` gets `reply`, unless its address is scripted.
    static void by_type(const RecordType qtype, ScriptedReply reply) {
        script().by_type[static_cast<std::uint16_t>(qtype)] = std::move(reply);
    }

    /// `server` gets `reply` for every query type.
    static void by_address(std::string server, ScriptedReply reply) {
        script().by_address[std::move(server)] = std::move(reply);
    }

    [[nodiscard]] static std::size_t call_count() { return calls().size(); }

    [[nodiscard]] static std::size_t calls_for(const std::string_view server) {
        std::size_t count = 0;
        for (const auto& call : calls()) {
            count += call.server == server ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] static std::size_t last_type() { return calls().empty() ? 0U : calls().back().qtype; }
};

/// The RDATA of an A answer.
[[nodiscard]] std::vector<std::uint8_t> v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return {a, b, c, d};
}

/// The RDATA of a 2001:db8::x AAAA answer.
[[nodiscard]] std::vector<std::uint8_t> v6(const std::uint8_t last) {
    return {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, last};
}

/// An address answer of the requested type.
[[nodiscard]] AnswerRecord address_answer(const RecordType type, std::vector<std::uint8_t> rdata) {
    return AnswerRecord{type, std::move(rdata)};
}

/// An A answer whose RDATA is one octet short of the four an address needs.
[[nodiscard]] AnswerRecord short_address_answer() {
    return AnswerRecord{RecordType::A, {198, 51, 100}};
}

/// The response flags: QR, RD, RA, and the RCODE in the low four bits.
[[nodiscard]] constexpr std::uint16_t header_flags(const Rcode rcode) {
    return static_cast<std::uint16_t>(0x8000U | 0x0100U | 0x0080U | (static_cast<std::uint16_t>(rcode) & 0x0FU));
}

/// Offset just past the question section of a well-formed query.
[[nodiscard]] std::size_t question_end(const std::span<const std::uint8_t> query) {
    std::size_t offset = dns::HEADER_SIZE;
    while (offset < query.size() && query[offset] != 0) {
        offset += static_cast<std::size_t>(query[offset]) + 1;
    }
    return offset + 1 + 4;  // root label, QTYPE, QCLASS
}

/// Build a response around the query's own question section, so the answer
/// owner always matches the name that was asked for.
[[nodiscard]] std::vector<std::uint8_t> build_response(const std::span<const std::uint8_t> query,
                                                       const ScriptedReply& reply) {
    const auto u16 = [](std::vector<std::uint8_t>& out, const std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    };
    const auto u32 = [](std::vector<std::uint8_t>& out, const std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value >> 24));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    };

    std::vector<std::uint8_t> out{0x12, 0x34};  // id (unused: the exchange matches)
    u16(out, header_flags(reply.rcode));
    u16(out, 1);
    u16(out, static_cast<std::uint16_t>(reply.answers.size()));
    u16(out, 0);
    u16(out, 0);
    out.insert(out.end(), query.begin() + dns::HEADER_SIZE,
               query.begin() + static_cast<std::ptrdiff_t>(question_end(query)));
    for (const auto& answer : reply.answers) {
        u16(out, 0xC00C);  // the question's name, at offset 12
        u16(out, static_cast<std::uint16_t>(answer.type));
        u16(out, 1);   // class IN
        u32(out, 60);  // TTL
        u16(out, static_cast<std::uint16_t>(answer.rdata.size()));
        out.insert(out.end(), answer.rdata.begin(), answer.rdata.end());
    }
    return out;
}

/// A response that claims a question section it does not carry.
[[nodiscard]] std::vector<std::uint8_t> truncated_header() {
    return {0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

/// Run a bootstrap lookup inside a fresh loop.
[[nodiscard]] ResolveResult resolve(std::string host, const std::optional<domain::AddressFamily> family,
                                    std::vector<domain::DnsServer> servers, const dns::Policy policy = {}) {
    return coro::run(dns::bootstrap_resolve(std::move(host), family, std::move(servers), policy));
}

/// A policy whose concurrency and collection window the case chooses.
[[nodiscard]] dns::Policy bootstrap_policy(const std::size_t max_concurrent = 3) {
    dns::Policy policy;
    policy.max_concurrent = max_concurrent;
    policy.family_collection_window = std::chrono::milliseconds{1000};  // the cases settle long before it
    return policy;
}

}  // namespace

namespace dns::detail {

/// In-memory replacement for the classic wire exchange. bootstrap owns its
/// sockets and gives the caller no factory, so this binary — and only this
/// binary — answers in its place; see the file header.
coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query_classic(
    domain::InetAddress server, std::uint16_t port, std::span<const std::uint8_t> query, Policy policy) {
    (void) port;
    (void) policy;
    const auto asked = static_cast<std::uint16_t>(RecordParser::parse_question(query).qtype);
    calls().push_back(ExchangeCall{server.to_string(), asked});

    const auto& installed = script();
    const auto exact = installed.by_address.find(server.to_string());
    const auto by_type = installed.by_type.find(asked);
    const ScriptedReply* reply = nullptr;
    if (exact != installed.by_address.end()) {
        reply = &exact->second;
    } else if (by_type != installed.by_type.end()) {
        reply = &by_type->second;
    }
    if (reply == nullptr) {
        // No script: refuse loudly rather than reach for a socket.
        co_return std::unexpected(
            domain::DnsErrorInfo{domain::DnsError::CONNECTION, "the in-memory exchange has no scripted reply"});
    }
    if (reply->failure) {
        co_return std::unexpected(*reply->failure);
    }
    if (!reply->raw.empty()) {
        co_return reply->raw;
    }
    co_return build_response(query, *reply);
}

}  // namespace dns::detail

namespace {

// ---------------------------------------------------------------------------
// configuration and local input checks: no exchange call at all
// ---------------------------------------------------------------------------

TEST(BootstrapPaths, emptyServerListIsAConfigError) {
    Wire wire;
    const auto result = resolve("yaddnsc.test", std::nullopt, {}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result.error().message.find("no bootstrap DNS servers"), std::string::npos);
    EXPECT_NE(result.error().message.find("yaddnsc.test"), std::string::npos);
    EXPECT_EQ(Wire::call_count(), 0U);
}

TEST(BootstrapPaths, zeroConcurrencyIsRejectedBeforeAnyLookup) {
    Wire wire;
    const auto result = resolve("yaddnsc.test", std::nullopt, {{"192.0.2.1", 53}}, bootstrap_policy(0));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result.error().message.find("concurrency must be positive"), std::string::npos);
    EXPECT_EQ(Wire::call_count(), 0U);
}

TEST(BootstrapPaths, nonLiteralServerIsSkippedWithoutTouchingTheExchange) {
    Wire wire;
    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"not-an-ip", 53}}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result.error().message.find("must be an IP literal"), std::string::npos);
    EXPECT_EQ(Wire::call_count(), 0U);
}

TEST(BootstrapPaths, zeroPortServerIsSkippedWithoutTouchingTheExchange) {
    Wire wire;
    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"192.0.2.1", 0}}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result.error().message.find("nonzero port"), std::string::npos);
    EXPECT_EQ(Wire::call_count(), 0U);
}

TEST(BootstrapPaths, literalHostBypassesTheExchange) {
    Wire wire;
    const auto result = resolve("198.51.100.7", std::nullopt, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->at(0).to_string(), "198.51.100.7");
    EXPECT_EQ(Wire::call_count(), 0U);
}

TEST(BootstrapPaths, literalHostOfTheOtherFamilyIsALocalMismatch) {
    Wire wire;
    const auto result = resolve("::1", domain::AddressFamily::IPV4, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_NE(result.error().message.find("does not match the requested address family"), std::string::npos);
    EXPECT_EQ(Wire::call_count(), 0U);
}

// ---------------------------------------------------------------------------
// one server's response
// ---------------------------------------------------------------------------

TEST(BootstrapPaths, addressAnswerIsReturnedAsAnAddress) {
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{.answers = {address_answer(RecordType::A, v4(198, 51, 100, 7))}});

    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->at(0).to_string(), "198.51.100.7");
    EXPECT_EQ(Wire::call_count(), 1U);
    EXPECT_EQ(Wire::last_type(), static_cast<std::size_t>(RecordType::A));
}

TEST(BootstrapPaths, malformedResponseIsAParseFailure) {
    // A response the parser rejects must surface as PARSE — the endpoint's
    // verdict — not as NODATA, which would claim the name simply has no record.
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{.raw = truncated_header()});

    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_FALSE(result.error().message.empty());
}

TEST(BootstrapPaths, answerWhoseRdataLengthContradictsItsTypeIsAParseError) {
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{.answers = {short_address_answer()}});

    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_NE(result.error().message.find("RDATA length"), std::string::npos);
}

TEST(BootstrapPaths, emptyAnswerSectionIsReportedAsNodataNamingTheHost) {
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{});

    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::NODATA);
    // A NODATA diagnosis already names the host: it must not be named twice.
    EXPECT_NE(result.error().message.find("returned no usable records"), std::string::npos);
    EXPECT_EQ(result.error().message.find("Cannot resolve"), std::string::npos);
}

TEST(BootstrapPaths, nxdomainNamesTheHostOnce) {
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{.rcode = Rcode::NXDOMAIN});

    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::NX_DOMAIN);
    EXPECT_EQ(result.error().message.find("Cannot resolve"), std::string::npos);
    EXPECT_NE(result.error().message.find("NXDOMAIN"), std::string::npos);
}

TEST(BootstrapPaths, transportFailureIsReportedAsTheHostThatCouldNotBeResolved) {
    Wire wire;
    Wire::by_type(RecordType::A,
                  ScriptedReply{.failure = domain::DnsErrorInfo{domain::DnsError::RETRY, "wire budget exhausted"}});

    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::RETRY);
    // Per-server transport detail collapses into the bare summary at this boundary.
    EXPECT_EQ(result.error().message, R"(Cannot resolve "yaddnsc.test")");
}

// ---------------------------------------------------------------------------
// the server race
// ---------------------------------------------------------------------------

TEST(BootstrapPaths, negativeAnswerFromTheFirstServerLetsTheSecondAnswer) {
    Wire wire;
    Wire::by_address("192.0.2.1", ScriptedReply{.rcode = Rcode::NXDOMAIN});
    Wire::by_address("192.0.2.2", ScriptedReply{.answers = {address_answer(RecordType::A, v4(198, 51, 100, 7))}});

    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"192.0.2.1", 53}, {"192.0.2.2", 53}},
                                bootstrap_policy(1));

    ASSERT_TRUE(result);
    EXPECT_EQ(result->at(0).to_string(), "198.51.100.7");
    EXPECT_EQ(Wire::call_count(), 2U);
}

TEST(BootstrapPaths, transientFailureFreesTheSlotForTheNextServer) {
    Wire wire;
    Wire::by_address("192.0.2.1",
                     ScriptedReply{.failure = domain::DnsErrorInfo{domain::DnsError::RETRY, "wire budget exhausted"}});
    Wire::by_address("192.0.2.2", ScriptedReply{.answers = {address_answer(RecordType::A, v4(198, 51, 100, 7))}});

    const auto result = resolve("yaddnsc.test", domain::AddressFamily::IPV4, {{"192.0.2.1", 53}, {"192.0.2.2", 53}},
                                bootstrap_policy(1));

    ASSERT_TRUE(result);
    EXPECT_EQ(result->at(0).to_string(), "198.51.100.7");
    EXPECT_EQ(Wire::calls_for("192.0.2.1"), 1U);
    EXPECT_EQ(Wire::calls_for("192.0.2.2"), 1U);
}

TEST(BootstrapPaths, invalidHostIsInvalidQueryBeforeAnyServerIsAsked) {
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{.answers = {address_answer(RecordType::A, v4(198, 51, 100, 7))}});

    // The name cannot be encoded, so no query exists to send: the local defect
    // is INVALID_QUERY and it ends the search instead of failing over.
    const auto result = resolve(std::string(64, 'x') + ".test", domain::AddressFamily::IPV4,
                                {{"192.0.2.1", 53}, {"192.0.2.2", 53}}, bootstrap_policy(1));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::INVALID_QUERY);
    EXPECT_EQ(Wire::call_count(), 0U);
}

// ---------------------------------------------------------------------------
// the two address families
// ---------------------------------------------------------------------------

TEST(BootstrapPaths, bothFamiliesAreCollected) {
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{.answers = {address_answer(RecordType::A, v4(198, 51, 100, 7))}});
    Wire::by_type(RecordType::AAAA, ScriptedReply{.answers = {address_answer(RecordType::AAAA, v6(7))}});

    const auto result = resolve("yaddnsc.test", std::nullopt, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ(result->at(0).to_string(), "198.51.100.7");
    EXPECT_EQ(result->at(1).to_string(), "2001:db8::7");
}

TEST(BootstrapPaths, aSilentFamilyDoesNotDiscardTheAnsweredOne) {
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{.answers = {address_answer(RecordType::A, v4(198, 51, 100, 7))}});
    Wire::by_type(RecordType::AAAA, ScriptedReply{.rcode = Rcode::NXDOMAIN});

    const auto result = resolve("yaddnsc.test", std::nullopt, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->at(0).to_string(), "198.51.100.7");
}

TEST(BootstrapPaths, theHigherPriorityFamilyFailureIsTheOneReported) {
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{.rcode = Rcode::NXDOMAIN});     // the weakest verdict
    Wire::by_type(RecordType::AAAA, ScriptedReply{.rcode = Rcode::SERVFAIL});  // outranks it

    const auto result = resolve("yaddnsc.test", std::nullopt, {{"192.0.2.1", 53}}, bootstrap_policy());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::RETRY);
    EXPECT_EQ(result.error().message, R"(Cannot resolve "yaddnsc.test")");
}

// ---------------------------------------------------------------------------
// the injection port
// ---------------------------------------------------------------------------

TEST(BootstrapPaths, injectedResolverRunsTheSamePolicy) {
    Wire wire;
    Wire::by_type(RecordType::A, ScriptedReply{.answers = {address_answer(RecordType::A, v4(198, 51, 100, 42))}});
    const auto resolver = dns::make_bootstrap_resolver({{"192.0.2.1", 53}}, bootstrap_policy());

    const auto literal = coro::run(resolver("198.51.100.9", std::nullopt));
    ASSERT_TRUE(literal);
    EXPECT_EQ(literal->at(0).to_string(), "198.51.100.9");

    const auto looked_up = coro::run(resolver("yaddnsc.test", domain::AddressFamily::IPV4));
    ASSERT_TRUE(looked_up);
    EXPECT_EQ(looked_up->at(0).to_string(), "198.51.100.42");
    EXPECT_EQ(Wire::call_count(), 1U);
}

}  // namespace
