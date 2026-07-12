//
// Shared unit tests for ResolverDispatcher (both backends).
//
// This header is included by two translation units that compile against
// different DNS backends:
//   - test/unit/dispatcher.cpp        (YADDNSC_USE_NATIVE_DNS=1, jthread-based, default)
//   - test/unit/dispatcher_system.cpp (libresolv-based legacy backend, DEPRECATED)
//
// The two backends share identical dispatch *logic* but diverge in how they
// report failures when no definitive error is present:
//   * Native backend  → returns std::unexpected<DnsErrorInfo> (error code kept).
//   * System backend  → collapses single-resolver and all-retryable multi-
//                       resolver failures into an expected holding an EMPTY
//                       vector (the error code is lost — legacy quirk).
//
// The assertion helpers below branch on YADDNSC_NATIVE_DISPATCHER so the same
// test bodies validate both behaviours without duplicating logic.
// =============================================================================

#ifndef YADDNSC_TEST_FIXTURES_DISPATCHER_TESTS_H
#define YADDNSC_TEST_FIXTURES_DISPATCHER_TESTS_H

#include <array>
#include <cstdint>
#include <expected>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "config/dns_config.h"
#include "dns/dispatcher.h"
#include "dns/dns_error_info.h"
#include "dns/resolver/base.h"
#include "dns_error.h"
#include "exception/dns_lookup.h"
#include "mocks/mock_resolver.h"
#include "record_kind.h"

#if YADDNSC_USE_NATIVE_DNS
#  define YADDNSC_NATIVE_DISPATCHER 1
#else
#  define YADDNSC_NATIVE_DISPATCHER 0
#endif

namespace {

using ::testing::_;
using ::testing::Return;

// ── DNS wire-format packet builders (backend-agnostic) ───────────────────────

void write_u16_be(std::vector<std::uint8_t> &buf, std::size_t offset, std::uint16_t v) {
    buf[offset] = static_cast<std::uint8_t>(v >> 8);
    buf[offset + 1] = static_cast<std::uint8_t>(v & 0xFF);
}

std::size_t encode_name(std::vector<std::uint8_t> &buf, std::string_view name) {
    std::size_t written = 0;
    std::size_t pos = 0;
    while (pos < name.size()) {
        auto dot = name.find('.', pos);
        if (dot == std::string::npos) dot = name.size();
        auto label_len = static_cast<std::uint8_t>(dot - pos);
        buf.push_back(label_len);
        ++written;
        for (std::size_t i = 0; i < label_len; ++i) {
            buf.push_back(static_cast<std::uint8_t>(name[pos + i]));
            ++written;
        }
        pos = dot + 1;
    }
    buf.push_back(0);
    ++written;
    return written;
}

// Standard response header with a single question and `ancount` answers.
// `rcode_byte` carries the full third header byte (flags + RCODE in low nibble).
std::vector<std::uint8_t> make_header_response(std::uint16_t txid, std::uint8_t rcode_byte,
                                               std::uint16_t ancount) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, txid);
    buf[2] = 0x81;
    buf[3] = rcode_byte;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, ancount);
    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    return buf;
}

std::vector<std::uint8_t> make_a_response(std::uint16_t txid, std::array<std::uint8_t, 4> ip,
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
    buf.push_back(ip[0]);
    buf.push_back(ip[1]);
    buf.push_back(ip[2]);
    buf.push_back(ip[3]);
    return buf;
}

std::vector<std::uint8_t> make_aaaa_response(std::uint16_t txid, std::array<std::uint8_t, 16> ip,
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
    for (std::uint8_t b: ip) buf.push_back(b);
    return buf;
}

// Header-only response carrying a specific RCODE (low nibble of byte 3) and no
// answer records. Used to exercise try_resolve()'s RCODE classification.
std::vector<std::uint8_t> make_rcode_response(std::uint8_t rcode_byte) {
    return make_header_response(0x1234, rcode_byte, 0);
}

// A response with two A records (exercises the >1 address warning branch).
std::vector<std::uint8_t> make_multi_a_response(std::uint16_t txid) {
    std::vector<std::uint8_t> buf;
    buf.resize(12, 0);
    write_u16_be(buf, 0, txid);
    buf[2] = 0x81;
    buf[3] = 0x80;
    write_u16_be(buf, 4, 1);
    write_u16_be(buf, 6, 2);
    encode_name(buf, "example.com");
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    // record 1
    buf.push_back(0xC0);
    buf.push_back(0x0C);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x2C);
    buf.push_back(0x00);
    buf.push_back(0x04);
    buf.push_back(192);
    buf.push_back(168);
    buf.push_back(1);
    buf.push_back(1);
    // record 2
    buf.push_back(0xC0);
    buf.push_back(0x0C);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x01);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x2C);
    buf.push_back(0x00);
    buf.push_back(0x04);
    buf.push_back(192);
    buf.push_back(168);
    buf.push_back(1);
    buf.push_back(2);
    return buf;
}

// Truncated packet — both parsers must reject it (PARSE).
std::vector<std::uint8_t> make_malformed_response() {
    return std::vector<std::uint8_t>{0x12, 0x34, 0x81, 0x80, 0x00, 0x01};
}

// ── Resolver result builders ─────────────────────────────────────────────────

std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
ok_a(std::array<std::uint8_t, 4> ip = {192, 168, 1, 1}) {
    return make_a_response(0x1234, ip);
}

std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
ok_aaaa(std::array<std::uint8_t, 16> ip = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}) {
    return make_aaaa_response(0x1234, ip);
}

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> err(DnsError code, std::string_view msg) {
    return std::unexpected(DnsErrorInfo{code, std::string(msg)});
}

// ── Mock resolver factory (sets a default get_type so logging never warns) ───

std::unique_ptr<::testing::NiceMock<MockResolver>> make_mock() {
    auto r = std::make_unique<::testing::NiceMock<MockResolver>>();
    ON_CALL(*r, get_type()).WillByDefault(Return("Mock"));
    return r;
}

// ── Assertion helpers (branch on backend divergence) ─────────────────────────
//
// NOTE: these use EXPECT_* (not ASSERT_*) so a failed precondition reports
// without an early return from the helper. Dereferencing of the expected is
// always guarded so a wrong-state result cannot crash the test.

// Failure where every resolver returned only retryable/transient errors and no
// definitive error was produced.
void expect_transient_failure(const std::expected<std::vector<std::string>, DnsErrorInfo> &r) {
#if YADDNSC_NATIVE_DISPATCHER
    EXPECT_FALSE(r.has_value());
#else
    EXPECT_TRUE(r.has_value());
    if (r.has_value()) {
        EXPECT_TRUE(r->empty());
    }
#endif
}

// Single-resolver failure (definitive or not). The native backend preserves the
// error code; the legacy system backend collapses it to an empty-vector value.
void expect_single_resolver_failure(const std::expected<std::vector<std::string>, DnsErrorInfo> &r,
                                    DnsError code) {
#if YADDNSC_NATIVE_DISPATCHER
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, code);
    }
#else
    EXPECT_TRUE(r.has_value());
    if (r.has_value()) {
        EXPECT_TRUE(r->empty());
    }
#endif
}

// Multi-resolver definitive failure — both backends report std::unexpected.
void expect_unexpected(const std::expected<std::vector<std::string>, DnsErrorInfo> &r, DnsError code) {
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_EQ(r.error().code, code);
    }
}

// ── A resolver that blocks on cancel_fd until cancelled (or poll timeout) ─────
//
// Used to verify the concurrent pipeline's cancellation pipe wakes a slower
// resolver once a faster one has already produced a result. Both backends pass
// the cancellation read fd through to query(), so this exercises the real
// cancellation path on each.
class SlowCancellableResolver : public ResolverBase {
public:
    explicit SlowCancellableResolver(bool succeed, int poll_ms = 3000)
        : succeed_(succeed), poll_ms_(poll_ms) {}

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &, RecordKind, const Utils::CancellationToken &cancel_token) const override {
        if (cancel_token) {
            const int cancel_fd = cancel_token.native_handle();
            ::pollfd pfd{cancel_fd, POLLIN, 0};
            const int r = ::poll(&pfd, 1, poll_ms_);
            if (r > 0) {
                cancel_token.drain();
                return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "cancelled by winner"});
            }
        }
        if (succeed_) {
            return ok_a();
        }
        return std::unexpected(DnsErrorInfo{DnsError::RETRY, "retry"});
    }

    [[nodiscard]] std::string_view get_type() const noexcept override { return "Slow"; }

private:
    bool succeed_;
    int poll_ms_;
};

} // namespace

// =============================================================================
//  Single-resolver mode (exactly one backend — retry applies)
// =============================================================================

TEST(DispatcherSingle, Success_ReturnsResolvedAddress) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(ok_a()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

TEST(DispatcherSingle, Aaaa_Success_ReturnsIpv6Address) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(ok_aaaa()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::AAAA, 5, 1);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ((*result)[0], "2001:db8::1");
}

TEST(DispatcherSingle, RetriesOnTransientThenSucceeds) {
    auto r = make_mock();
    EXPECT_CALL(*r, query(_, _, _))
        .WillOnce(Return(err(DnsError::RETRY, "t1")))
        .WillOnce(Return(err(DnsError::RETRY, "t2")))
        .WillOnce(Return(ok_a()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

TEST(DispatcherSingle, ExhaustsRetries_ReturnsFailure) {
    auto r = make_mock();
    EXPECT_CALL(*r, query(_, _, _))
        .Times(4) // 1 initial attempt + 3 retries (max_retries = 3)
        .WillRepeatedly(Return(err(DnsError::RETRY, "retry")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 3, 1);
    expect_transient_failure(result);
}

TEST(DispatcherSingle, NxDomain_ReturnsFailure) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(err(DnsError::NX_DOMAIN, "nxdomain")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    expect_single_resolver_failure(result, DnsError::NX_DOMAIN);
}

TEST(DispatcherSingle, Nodata_ReturnsFailure) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(err(DnsError::NODATA, "nodata")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    expect_single_resolver_failure(result, DnsError::NODATA);
}

TEST(DispatcherSingle, ParseError_ReturnsFailure) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(make_malformed_response()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    expect_single_resolver_failure(result, DnsError::PARSE);
}

TEST(DispatcherSingle, ZeroRetries_AttemptsOnce) {
    auto r = make_mock();
    EXPECT_CALL(*r, query(_, _, _))
        .Times(1)
        .WillRepeatedly(Return(err(DnsError::RETRY, "retry")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 0, 1);
    expect_transient_failure(result);
}

// ── RCODE classification in try_resolve() (raw packet, not pre-built error) ──
// These exercise the RCODE→DnsError mapping that only triggers when a resolver
// returns a real wire-format response with a non-NOERROR RCODE.

TEST(DispatcherSingle, RcodeNxDomain_Classified) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(make_rcode_response(0x83)));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    expect_single_resolver_failure(result, DnsError::NX_DOMAIN);
}

TEST(DispatcherSingle, RcodeServfail_IsRetryable) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(make_rcode_response(0x82)));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 3, 1);
    expect_transient_failure(result);
}

TEST(DispatcherSingle, RcodeRefused_Classified) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(make_rcode_response(0x85)));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    expect_single_resolver_failure(result, DnsError::SERVER_REFUSED);
}

TEST(DispatcherSingle, RcodeNodata_Classified) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(make_rcode_response(0x80)));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    expect_single_resolver_failure(result, DnsError::NODATA);
}

TEST(DispatcherSingle, RcodeUnknown_DefaultBranch) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(make_rcode_response(0x86))); // NOTIMP
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    expect_single_resolver_failure(result, DnsError::UNKNOWN);
}

TEST(DispatcherSingle, MultipleRecords_ReturnsAll) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(make_multi_a_response(0x1234)));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ((*result)[0], "192.168.1.1");
    EXPECT_EQ((*result)[1], "192.168.1.2");
}

// ── Exception boundary translation in try_resolve() ──

// A resolver that throws DnsLookupException — must be translated to PARSE.
// Both backends catch the exception at the resolve boundary and return
// std::unexpected (the legacy empty-vector collapse only applies to
// resolver-returned errors, not to exceptions).
TEST(DispatcherSingle, ThrowsDnsLookupException_TranslatedToParse) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _))
        .WillByDefault([](const std::string &, RecordKind,
                          const Utils::CancellationToken &) -> std::expected<std::vector<std::uint8_t>, DnsErrorInfo> {
            throw DnsLookupException("parse boom", DnsError::PARSE);
        });
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    expect_unexpected(result, DnsError::PARSE);
}

// A resolver that throws a generic std::exception — must be translated to UNKNOWN.
TEST(DispatcherSingle, ThrowsStdException_TranslatedToUnknown) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _))
        .WillByDefault([](const std::string &, RecordKind,
                          const Utils::CancellationToken &) -> std::expected<std::vector<std::uint8_t>, DnsErrorInfo> {
            throw std::runtime_error("transport boom");
        });
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    expect_unexpected(result, DnsError::UNKNOWN);
}

// =============================================================================
//  Fallback strategy (size > 1, sequential)
// =============================================================================

TEST(DispatcherFallback, AnySucceeds_ReturnsRecords) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r0")));
    ON_CALL(*r1, query(_, _, _)).WillByDefault(Return(ok_a()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::FALLBACK);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

TEST(DispatcherFallback, NxDomain_StopsIteration) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r0")));
    ON_CALL(*r1, query(_, _, _)).WillByDefault(Return(err(DnsError::NX_DOMAIN, "nxdomain")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::FALLBACK);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    expect_unexpected(result, DnsError::NX_DOMAIN);
}

TEST(DispatcherFallback, AllRetryable_ReturnsTransientFailure) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r0")));
    ON_CALL(*r1, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r1")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::FALLBACK);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    expect_transient_failure(result);
}

TEST(DispatcherFallback, AllNodata_ReturnsTransientFailure) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _, _)).WillByDefault(Return(err(DnsError::NODATA, "nd0")));
    ON_CALL(*r1, query(_, _, _)).WillByDefault(Return(err(DnsError::NODATA, "nd1")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::FALLBACK);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    expect_transient_failure(result);
}

// =============================================================================
//  Concurrent strategy (size > 1, batched)
// =============================================================================

TEST(DispatcherConcurrent, AnySucceeds_ReturnsRecords) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    auto r2 = make_mock();
    ON_CALL(*r0, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r0")));
    ON_CALL(*r1, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r1")));
    ON_CALL(*r2, query(_, _, _)).WillByDefault(Return(ok_a()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    resolvers.push_back(std::move(r2));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

TEST(DispatcherConcurrent, NxDomain_ReturnsNxDomain) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    auto r2 = make_mock();
    ON_CALL(*r0, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r0")));
    ON_CALL(*r1, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r1")));
    ON_CALL(*r2, query(_, _, _)).WillByDefault(Return(err(DnsError::NX_DOMAIN, "nxdomain")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    resolvers.push_back(std::move(r2));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    expect_unexpected(result, DnsError::NX_DOMAIN);
}

TEST(DispatcherConcurrent, AllRetryable_ReturnsTransientFailure) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    auto r2 = make_mock();
    ON_CALL(*r0, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r0")));
    ON_CALL(*r1, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r1")));
    ON_CALL(*r2, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r2")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    resolvers.push_back(std::move(r2));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    expect_transient_failure(result);
}

// Verifies the cancellation pipe wakes a slower resolver once a faster one wins,
// so the dispatch returns promptly instead of blocking on the slow backend.
TEST(DispatcherConcurrent, SlowResolverCancelledByWinner) {
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::make_unique<SlowCancellableResolver>(true));  // fast winner
    resolvers.push_back(std::make_unique<SlowCancellableResolver>(false)); // slow, will be cancelled
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

// More resolvers than MAX_CONCURRENT_RESOLVERS (3) — exercises batching.
TEST(DispatcherConcurrent, MoreThanMax_AllRetryable) {
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    for (int i = 0; i < 5; ++i) {
        auto r = make_mock();
        ON_CALL(*r, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r")));
        resolvers.push_back(std::move(r));
    }
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    expect_transient_failure(result);
}

// A resolver in a later batch can still win after earlier batches all fail.
TEST(DispatcherConcurrent, MoreThanMax_OneSucceedsInLaterBatch) {
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    for (int i = 0; i < 3; ++i) {
        auto r = make_mock();
        ON_CALL(*r, query(_, _, _)).WillByDefault(Return(err(DnsError::RETRY, "r")));
        resolvers.push_back(std::move(r));
    }
    auto winner = make_mock();
    ON_CALL(*winner, query(_, _, _)).WillByDefault(Return(ok_a()));
    resolvers.push_back(std::move(winner));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

// =============================================================================
//  Strategy selection & ownership
// =============================================================================

// With exactly one resolver the configured strategy is irrelevant — the single
// path (with retry) is always taken.
TEST(DispatcherStrategy, SingleResolverIgnoresStrategy) {
    auto r = make_mock();
    EXPECT_CALL(*r, query(_, _, _))
        .WillOnce(Return(err(DnsError::RETRY, "t1")))
        .WillOnce(Return(ok_a()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::FALLBACK);

    auto result = disp.resolve("example.com", RecordKind::A, 5, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

TEST(Dispatcher, MoveConstructible) {
    auto r = make_mock();
    ON_CALL(*r, query(_, _, _)).WillByDefault(Return(ok_a()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    ResolverDispatcher moved = std::move(disp);
    auto result = moved.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

#endif // YADDNSC_TEST_FIXTURES_DISPATCHER_TESTS_H
