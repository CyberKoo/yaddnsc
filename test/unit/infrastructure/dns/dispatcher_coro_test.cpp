//
// Unit tests for the coroutine DNS dispatcher, driven by scripted fake
// resolvers. No sockets: the strategies, their retry policy, the concurrent
// race and the error-classification rules are all exercised in memory.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "coro/coro.h"
#include "infrastructure/dns/dispatcher.h"
#include "infrastructure/dns/resolver/resolver.h"

namespace {

using namespace std::chrono_literals;

using dns::Strategy;

/// Build a minimal A-record response for `name`.
[[nodiscard]] std::vector<std::uint8_t> a_response(const std::string_view name, const std::uint8_t last_octet) {
    std::vector<std::uint8_t> out;
    const auto u16 = [&out](const std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    };
    const auto u32 = [&out](const std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value >> 24));
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    };

    u16(0x1234);  // id
    u16(0x8180);  // QR | RD | RA
    u16(1);       // qdcount
    u16(1);       // ancount
    u16(0);
    u16(0);

    std::string_view rest = name;
    while (!rest.empty()) {
        const auto dot = rest.find('.');
        const auto label = rest.substr(0, dot);
        out.push_back(static_cast<std::uint8_t>(label.size()));
        out.insert(out.end(), label.begin(), label.end());
        if (dot == std::string_view::npos) {
            break;
        }
        rest = rest.substr(dot + 1);
    }
    out.push_back(0);  // root label
    u16(1);            // QTYPE A
    u16(1);            // QCLASS IN

    u16(0xC00C);  // name pointer to offset 12
    u16(1);       // TYPE A
    u16(1);       // CLASS IN
    u32(60);      // TTL
    u16(4);       // RDLENGTH
    out.push_back(198);
    out.push_back(51);
    out.push_back(100);
    out.push_back(last_octet);
    return out;
}

/// Build a header-only NXDOMAIN response for `name`.
[[nodiscard]] std::vector<std::uint8_t> nxdomain_response(const std::string_view name) {
    auto out = a_response(name, 0);
    out[3] = 0x83;  // RCODE = NXDOMAIN
    out[6] = 0x00;  // ANCOUNT = 0
    out[7] = 0x00;
    out.resize(out.size() - 16);  // name(2) + type(2) + class(2) + ttl(4) + rdlength(2) + rdata(4)
    return out;
}

/// One scripted reply: an error, a response, and an optional delay.
struct FakeStep {
    domain::DnsErrorInfo error{};
    std::vector<std::uint8_t> response{};
    std::chrono::milliseconds delay{};
};

/// A scripted failure reply.
[[nodiscard]] FakeStep error_step(domain::DnsError code, std::string message, std::chrono::milliseconds delay = {}) {
    FakeStep step;
    step.error = {code, std::move(message)};
    step.delay = delay;
    return step;
}

/// A scripted answer reply.
[[nodiscard]] FakeStep response_step(std::vector<std::uint8_t> response, std::chrono::milliseconds delay = {}) {
    FakeStep step;
    step.response = std::move(response);
    step.delay = delay;
    return step;
}

/// A resolver backend with a scripted reply per call.
class FakeResolver final : public dns::Resolver {
public:
    using Step = FakeStep;

    FakeResolver(std::string name, std::vector<Step> script) : name_(std::move(name)), script_(std::move(script)) {}

    [[nodiscard]] coro::Task<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> query(
        std::string host, domain::RecordKind kind) override {
        (void) host;
        (void) kind;
        ++calls_;
        const auto index = std::min(static_cast<std::size_t>(calls_ - 1), script_.size() - 1);
        const Step& step = script_[index];
        if (step.delay.count() > 0) {
            co_await coro::sleep_for(step.delay);
        }
        if (!step.response.empty()) {
            co_return step.response;
        }
        co_return std::unexpected(step.error);
    }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] int calls() const noexcept { return calls_; }

private:
    std::string name_;
    std::vector<Step> script_;
    int calls_{0};
};

/// Resolve through a dispatcher inside a fresh loop.
[[nodiscard]] std::expected<std::vector<std::string>, domain::DnsErrorInfo> run_query(
    dns::Dispatcher& dispatcher, std::string host = "yaddnsc.test", const std::uint32_t max_retries = 1) {
    return coro::run([&]() -> coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> {
        co_return co_await dispatcher.resolve(std::move(host), domain::RecordKind::A, max_retries);
    }());
}

/// Owns the fake backends and hands their ownership to a dispatcher, while
/// keeping references the test can assert on. The fakes are heap objects, so
/// the dispatcher's destruction is what releases them.
class Fakes {
public:
    FakeResolver& add(std::string name, std::vector<FakeResolver::Step> script) {
        auto fake = std::make_unique<FakeResolver>(std::move(name), std::move(script));
        FakeResolver& reference = *fake;
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
    std::vector<std::unique_ptr<FakeResolver>> owned_;
};

// ---------------------------------------------------------------------------
// single backend: retry policy
// ---------------------------------------------------------------------------

TEST(DnsDispatcher, singleResolver_retryableFailureThenAnswer_RetriesOnce) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& resolver = fakes.add(
        "only", {error_step(domain::DnsError::RETRY, "servfail"), response_step(a_response("yaddnsc.test", 42))});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK};

    const auto result = run_query(dispatcher, "yaddnsc.test", 1);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::string>{"198.51.100.42"}));
    EXPECT_EQ(resolver.calls(), 2);
}

TEST(DnsDispatcher, singleResolver_definitiveParseError_IsNotRetried) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& resolver = fakes.add("only", {error_step(domain::DnsError::PARSE, "malformed")});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK};

    const auto result = run_query(dispatcher, "yaddnsc.test", 3);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_EQ(resolver.calls(), 1);  // no retry for a definitive error
}

TEST(DnsDispatcher, singleResolver_retriesExhausted_ReportsTheLastError) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& resolver =
        fakes.add("only", {error_step(domain::DnsError::CONNECTION, "unreachable")});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK};

    const auto result = run_query(dispatcher, "yaddnsc.test", 2);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONNECTION);
    EXPECT_EQ(resolver.calls(), 3);  // initial attempt plus two retries
}

// ---------------------------------------------------------------------------
// sequential strategies
// ---------------------------------------------------------------------------

TEST(DnsDispatcher, fallback_firstBackendAnswers_StopsThere) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& first = fakes.add("first", {response_step(a_response("yaddnsc.test", 42))});
    [[maybe_unused]] FakeResolver& second = fakes.add("second", {response_step(a_response("yaddnsc.test", 43))});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK};

    const auto result = run_query(dispatcher);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::string>{"198.51.100.42"}));
    EXPECT_EQ(first.calls(), 1);
    EXPECT_EQ(second.calls(), 0);
}

TEST(DnsDispatcher, fallback_transientFailure_MovesToTheNextBackend) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& first = fakes.add("first", {error_step(domain::DnsError::CONNECTION, "down")});
    [[maybe_unused]] FakeResolver& second = fakes.add("second", {response_step(a_response("yaddnsc.test", 42))});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK};

    const auto result = run_query(dispatcher);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(first.calls(), 1);
    EXPECT_EQ(second.calls(), 1);
}

TEST(DnsDispatcher, fallback_nxdomain_StopsTheSearch) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& first = fakes.add("first", {response_step(nxdomain_response("yaddnsc.test"))});
    [[maybe_unused]] FakeResolver& second = fakes.add("second", {response_step(a_response("yaddnsc.test", 42))});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::NX_DOMAIN);
    EXPECT_EQ(second.calls(), 0);  // the name does not exist, so no other backend is asked
}

TEST(DnsDispatcher, shuffle_transientThenAnswer_StillResolves) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& first = fakes.add("first", {error_step(domain::DnsError::RETRY, "later")});
    [[maybe_unused]] FakeResolver& second = fakes.add("second", {response_step(a_response("yaddnsc.test", 42))});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::SHUFFLE};

    const auto result = run_query(dispatcher);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::string>{"198.51.100.42"}));
}

// ---------------------------------------------------------------------------
// concurrent strategy
// ---------------------------------------------------------------------------

TEST(DnsDispatcher, concurrent_fastestAnswerWins) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& slow = fakes.add("slow", {response_step(a_response("yaddnsc.test", 1), 60ms)});
    [[maybe_unused]] FakeResolver& fast = fakes.add("fast", {response_step(a_response("yaddnsc.test", 2), 1ms)});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::string>{"198.51.100.2"}));
    EXPECT_EQ(fast.calls(), 1);
}

TEST(DnsDispatcher, concurrent_definitiveErrorSurvivesALaterTransientOne) {
    // The transient failure lands first; the definitive PARSE must not be
    // downgraded by it, whichever order the race completes in.
    Fakes fakes;
    [[maybe_unused]] FakeResolver& transient =
        fakes.add("transient", {error_step(domain::DnsError::RETRY, "servfail", 1ms)});
    [[maybe_unused]] FakeResolver& definitive =
        fakes.add("definitive", {error_step(domain::DnsError::PARSE, "malformed", 20ms)});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
}

TEST(DnsDispatcher, concurrent_nxdomainPreferredOverTransient) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& transient =
        fakes.add("transient", {error_step(domain::DnsError::RETRY, "servfail", 1ms)});
    [[maybe_unused]] FakeResolver& missing =
        fakes.add("missing", {response_step(nxdomain_response("yaddnsc.test"), 20ms)});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::NX_DOMAIN);
}

TEST(DnsDispatcher, concurrent_beyondThree_RefillsFreedSlots) {
    // Four backends for three slots: the first three all fail transiently, and
    // each failure frees its slot, so the fourth backend launches and answers.
    Fakes fakes;
    for (int i = 0; i < 3; ++i) {
        (void) fakes.add("down" + std::to_string(i), {error_step(domain::DnsError::RETRY, "down")});
    }
    FakeResolver& answers = fakes.add("answers", {response_step(a_response("yaddnsc.test", 7))});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::string>{"198.51.100.7"}));
    EXPECT_EQ(answers.calls(), 1);
}

TEST(DnsDispatcher, concurrent_freedSlotLaunchesTheNextBackendImmediately) {
    // A slow answer occupies one slot while two fast transient failures free
    // theirs: the fourth backend launches at the first failure and beats the
    // slow answer. Wave semantics would let the slow answer win instead.
    Fakes fakes;
    [[maybe_unused]] FakeResolver& down_a = fakes.add("down-a", {error_step(domain::DnsError::RETRY, "down", 1ms)});
    FakeResolver& slow = fakes.add("slow-answer", {response_step(a_response("yaddnsc.test", 2), 30ms)});
    [[maybe_unused]] FakeResolver& down_b = fakes.add("down-b", {error_step(domain::DnsError::RETRY, "down", 1ms)});
    FakeResolver& answers = fakes.add("answers", {response_step(a_response("yaddnsc.test", 7), 1ms)});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, (std::vector<std::string>{"198.51.100.7"}));
    EXPECT_EQ(slow.calls(), 1);
    EXPECT_EQ(answers.calls(), 1);
}

TEST(DnsDispatcher, concurrent_nxdomain_StopsFurtherLaunches) {
    // NXDOMAIN from one backend ends the search: the in-flight queries settle,
    // but the fourth backend is never launched.
    Fakes fakes;
    (void) fakes.add("missing", {response_step(nxdomain_response("yaddnsc.test"), 1ms)});
    (void) fakes.add("down-a", {error_step(domain::DnsError::RETRY, "down", 30ms)});
    (void) fakes.add("down-b", {error_step(domain::DnsError::RETRY, "down", 30ms)});
    FakeResolver& late = fakes.add("late", {response_step(a_response("yaddnsc.test", 7), 1ms)});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::NX_DOMAIN);
    EXPECT_EQ(late.calls(), 0);
}

TEST(DnsDispatcher, concurrent_definitive_StopsFurtherLaunches) {
    // A definitive PARSE failure ends the search the same way NXDOMAIN does.
    Fakes fakes;
    (void) fakes.add("malformed", {error_step(domain::DnsError::PARSE, "bad", 1ms)});
    (void) fakes.add("down-a", {error_step(domain::DnsError::RETRY, "down", 30ms)});
    (void) fakes.add("down-b", {error_step(domain::DnsError::RETRY, "down", 30ms)});
    FakeResolver& late = fakes.add("late", {response_step(a_response("yaddnsc.test", 7), 1ms)});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::PARSE);
    EXPECT_EQ(late.calls(), 0);
}

// ---------------------------------------------------------------------------
// RCODE classification
// ---------------------------------------------------------------------------

TEST(DnsDispatcher, servfail_IsClassifiedAsRetryable) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& resolver =
        fakes.add("only", {error_step(domain::DnsError::RETRY, "server said so")});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK};

    const auto result = run_query(dispatcher, "yaddnsc.test", 0);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::RETRY);
}

TEST(DnsDispatcher, noResolvers_IsAConfigError) {
    dns::Dispatcher dispatcher{{}, Strategy::CONCURRENT};

    const auto result = run_query(dispatcher);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
}

// ---------------------------------------------------------------------------
// cancellation
// ---------------------------------------------------------------------------

TEST(DnsDispatcher, scopeTimeout_AbortsTheQueryAsCancelled) {
    Fakes fakes;
    [[maybe_unused]] FakeResolver& resolver = fakes.add("slow", {response_step(a_response("yaddnsc.test", 42), 500ms)});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::FALLBACK};

    bool timed_out = false;
    std::optional<domain::DnsErrorInfo> error;
    coro::run([&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(20ms, [&]() -> coro::Task<void> {
            auto result = co_await dispatcher.resolve("yaddnsc.test", domain::RecordKind::A);
            if (!result) {
                error = result.error();
            }
            co_return;
        });
        timed_out = outcome.timed_out;
        co_return;
    }());

    EXPECT_TRUE(timed_out);
    EXPECT_FALSE(error.has_value());  // cancellation bypasses the recoverable error channel
}

TEST(DnsDispatcher, concurrent_cancellationLaunchesNothingNew) {
    // Five backends for three slots: shutdown cancels the in-flight queries,
    // and the two never-launched backends must stay uncalled.
    Fakes fakes;
    for (int i = 0; i < 3; ++i) {
        (void) fakes.add("slow" + std::to_string(i), {response_step(a_response("yaddnsc.test", 42), 500ms)});
    }
    FakeResolver& extra_a = fakes.add("extra-a", {response_step(a_response("yaddnsc.test", 7), 1ms)});
    FakeResolver& extra_b = fakes.add("extra-b", {response_step(a_response("yaddnsc.test", 8), 1ms)});
    dns::Dispatcher dispatcher{fakes.take(), Strategy::CONCURRENT};

    bool timed_out = false;
    std::optional<domain::DnsErrorInfo> error;
    coro::run([&]() -> coro::Task<void> {
        const auto outcome = co_await coro::with_timeout(20ms, [&]() -> coro::Task<void> {
            auto result = co_await dispatcher.resolve("yaddnsc.test", domain::RecordKind::A);
            if (!result) {
                error = result.error();
            }
            co_return;
        });
        timed_out = outcome.timed_out;
        co_return;
    }());

    EXPECT_TRUE(timed_out);
    EXPECT_FALSE(error.has_value());  // cancellation bypasses the recoverable error channel
    EXPECT_EQ(extra_a.calls(), 0);
    EXPECT_EQ(extra_b.calls(), 0);
}

}  // namespace
