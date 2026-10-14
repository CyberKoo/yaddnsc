//
// Unit tests for the dispatcher's remaining diagnosis and policy paths
// (src/infrastructure/dns/dispatcher.cpp): the concurrency guard in the
// constructor, the negative-answer summary the aggregate diagnosis appends,
// the recovery round that ends with a single candidate, and the race that
// finishes with no per-backend diagnosis at all.
//
// Everything runs on scripted in-memory resolvers: no socket is opened and no
// deadline is real.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "coro/coro.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/dispatcher.h"
#include "infrastructure/dns/policy.h"
#include "infrastructure/dns/resolver/resolver.h"

namespace {

using dns::Strategy;

using QueryResult = std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>;
using ResolveResult = std::expected<std::vector<std::string>, domain::DnsErrorInfo>;

/// A resolver backend with a scripted failure per call. The last step repeats,
/// so an unlimited retry budget still terminates.
class FakeResolver final : public dns::Resolver {
public:
    FakeResolver(std::string name, std::vector<domain::DnsErrorInfo> script)
        : name_(std::move(name)), script_(std::move(script)) {}

    [[nodiscard]] coro::Task<QueryResult> query(std::string host, domain::RecordKind kind) override {
        (void) host;
        (void) kind;
        ++calls_;
        const auto index = std::min(static_cast<std::size_t>(calls_ - 1), script_.size() - 1);
        co_return std::unexpected(script_[index]);
    }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] int calls() const noexcept { return calls_; }

private:
    std::string name_;
    std::vector<domain::DnsErrorInfo> script_;
    int calls_{0};
};

/// A backend that stops itself before it can report anything. A group skips a
/// cancelled child, so a race made only of these has no per-backend diagnosis
/// to aggregate.
class SelfCancellingResolver final : public dns::Resolver {
public:
    explicit SelfCancellingResolver(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] coro::Task<QueryResult> query(std::string host, domain::RecordKind kind) override {
        (void) host;
        (void) kind;
        ++calls_;
        (co_await coro::current_scope()).cancel();
        co_await coro::checkpoint();
        co_return std::vector<std::uint8_t>{};
    }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] int calls() const noexcept { return calls_; }

private:
    std::string name_;
    int calls_{0};
};

/// Owns the fake backends and hands their ownership to a dispatcher, while
/// keeping references the test can assert on. The fakes are heap objects, so
/// the dispatcher's destruction is what releases them.
class Fakes {
public:
    FakeResolver& add(std::string name, std::vector<domain::DnsErrorInfo> script) {
        auto fake = std::make_unique<FakeResolver>(std::move(name), std::move(script));
        FakeResolver& reference = *fake;
        owned_.push_back(std::move(fake));
        return reference;
    }

    SelfCancellingResolver& add_cancelling(std::string name) {
        auto fake = std::make_unique<SelfCancellingResolver>(std::move(name));
        SelfCancellingResolver& reference = *fake;
        owned_.push_back(std::move(fake));
        return reference;
    }

    [[nodiscard]] std::vector<std::unique_ptr<dns::Resolver>> take() {
        std::vector<std::unique_ptr<dns::Resolver>> resolvers;
        resolvers.reserve(owned_.size());
        for (auto& fake : owned_) {
            resolvers.push_back(std::move(fake));
        }
        return resolvers;
    }

private:
    std::vector<std::unique_ptr<dns::Resolver>> owned_;
};

/// Resolve through a dispatcher inside a fresh loop.
[[nodiscard]] ResolveResult run_query(dns::Dispatcher& dispatcher, std::string host = "yaddnsc.test") {
    return coro::run(dispatcher.resolve(std::move(host), domain::RecordKind::A));
}

/// Policy with the retry budget these cases ask for; the backoff is collapsed
/// so a recovery round costs no wall-clock time.
[[nodiscard]] dns::Policy retry_policy(const std::uint32_t max_retries) {
    dns::Policy policy;
    policy.max_retries = max_retries;
    policy.retry_backoff = std::chrono::milliseconds{0};
    return policy;
}

// ---------------------------------------------------------------------------
// constructor policy guard
// ---------------------------------------------------------------------------

TEST(DnsDispatcherPolicy, zeroMaxConcurrency_IsRejectedAtConstruction) {
    dns::Policy policy;
    policy.max_concurrent = 0;

    EXPECT_THROW(dns::Dispatcher({}, dns::Strategy::CONCURRENT, policy), std::invalid_argument);

    // The guard is exact: the smallest accepted limit is 1, even without backends.
    policy.max_concurrent = 1;
    EXPECT_NO_THROW(dns::Dispatcher({}, dns::Strategy::CONCURRENT, policy));
}

// ---------------------------------------------------------------------------
// aggregate diagnosis: the negative-answer summary
// ---------------------------------------------------------------------------

TEST(DnsDispatcher, nodataWithoutNxdomain_IsStillCountedInTheDiagnosis) {
    // A NODATA backend with no NXDOMAIN peer: the summary must appear for the
    // NODATA count alone, since PARSE outranks it as the reported failure.
    for (const auto strategy : {Strategy::FALLBACK, Strategy::CONCURRENT, Strategy::SHUFFLE}) {
        Fakes fakes;
        fakes.add("no-records", {domain::DnsErrorInfo{domain::DnsError::NODATA, "no records"}});
        fakes.add("malformed", {domain::DnsErrorInfo{domain::DnsError::PARSE, "bad response"}});
        dns::Dispatcher dispatcher{fakes.take(), strategy};

        const auto result = run_query(dispatcher);

        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
        EXPECT_NE(result.error().message.find("negative responses: NXDOMAIN=0, NODATA=1"), std::string::npos);
    }
}

TEST(DnsDispatcher, bothNegativeCounts_AreReportedOnce) {
    Fakes fakes;
    fakes.add("no-records", {domain::DnsErrorInfo{domain::DnsError::NODATA, "no records"}});
    fakes.add("missing", {domain::DnsErrorInfo{domain::DnsError::NX_DOMAIN, "nxdomain"}});
    fakes.add("malformed", {domain::DnsErrorInfo{domain::DnsError::PARSE, "bad response"}});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_NE(result.error().message.find("NXDOMAIN=1, NODATA=1"), std::string::npos);
    // The per-backend message is kept; the summary is appended to it.
    EXPECT_EQ(result.error().message.rfind("bad response", 0), 0U);
}

TEST(DnsDispatcher, nxdomainIsNotCountedAsAMissingRecord) {
    // A pure NXDOMAIN aggregate is the reported verdict itself: no summary, or
    // the same name-fact would be stated twice.
    Fakes fakes;
    fakes.add("missing-a", {domain::DnsErrorInfo{domain::DnsError::NX_DOMAIN, "nxdomain"}});
    fakes.add("missing-b", {domain::DnsErrorInfo{domain::DnsError::NX_DOMAIN, "nxdomain"}});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::NX_DOMAIN);
    EXPECT_EQ(result.error().message.find("negative responses"), std::string::npos);
}

// ---------------------------------------------------------------------------
// retry rounds
// ---------------------------------------------------------------------------

TEST(DnsDispatcher, recoveryRoundWithOneCandidate_KeepsTheEarlierDiagnosis) {
    // The first round leaves one transient candidate, so the recovery round
    // walks a single backend. The higher-priority PARSE the round already
    // collected must survive it rather than being replaced by the last error.
    Fakes fakes;
    auto& transient = fakes.add("transient", {domain::DnsErrorInfo{domain::DnsError::CONNECTION, "unreachable"}});
    auto& malformed = fakes.add("malformed", {domain::DnsErrorInfo{domain::DnsError::PARSE, "bad response"}});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK, retry_policy(1)};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_EQ(transient.calls(), 2);
    EXPECT_EQ(malformed.calls(), 1);
}

TEST(DnsDispatcher, retryableFailureIsNeverRetriedWithoutABudget) {
    Fakes fakes;
    auto& transient = fakes.add("transient", {domain::DnsErrorInfo{domain::DnsError::RETRY, "servfail"}});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK, retry_policy(0)};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::RETRY);
    EXPECT_EQ(transient.calls(), 1);
}

// ---------------------------------------------------------------------------
// no diagnosis at all
// ---------------------------------------------------------------------------

TEST(DnsDispatcher, raceWithoutAnyDiagnosis_ReportsTheAbsence) {
    // Every backend cancels itself, so the group yields no outcome and the
    // aggregate stays empty. An empty diagnosis must be reported as the absence
    // of one, not fabricated into a verdict (NODATA/NXDOMAIN) the DNS never gave.
    Fakes fakes;
    auto& first = fakes.add_cancelling("cancelled-a");
    auto& second = fakes.add_cancelling("cancelled-b");
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, domain::DnsError::UNKNOWN);
    EXPECT_NE(result.error().message.find("No resolver reported a diagnosis"), std::string::npos);
    EXPECT_EQ(first.calls(), 1);
    EXPECT_EQ(second.calls(), 1);
}

}  // namespace
