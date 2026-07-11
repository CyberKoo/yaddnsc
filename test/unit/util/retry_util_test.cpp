//
// Unit tests for util/retry_util.hpp — Utils::Retry::retry_on_error.
//
// Verifies:
//   - Returns immediately on success (no retries).
//   - Retries on error up to the configured limit.
//   - Returns success when a retry eventually succeeds.
//   - Exhausts retries and returns the last error.
//   - Error predicate filters which errors trigger a retry.
//   - actual_retries output parameter is set correctly.
//   - Zero retries means exactly one attempt, no backoff.
//   - The default backoff is applied.
//
// NOTE: retry_on_error has non-deducible template parameters R and E, so all
// calls must specify them explicitly: retry_on_error<R, E>(...)
// =============================================================================

#include <chrono>
#include <expected>

#include <gtest/gtest.h>

#include "util/retry_util.hpp"

// ── Helpers ───────────────────────────────────────────────────────────────────

enum class TestError {
    TRANSIENT,
    PERMANENT,
};

// Predicate that retries on any error (equivalent to std::nullopt default).
[[nodiscard]] inline bool retry_all(const TestError&) {
    return true;
}

// A callable that fails a configurable number of times before succeeding.
class FlakyCallable {
public:
    explicit FlakyCallable(unsigned fail_count, int value = 42)
        : fail_count_(fail_count), value_(value) {}

    std::expected<int, TestError> operator()() {
        if (attempts_++ < fail_count_) {
            return std::unexpected(TestError::TRANSIENT);
        }
        return value_;
    }

    unsigned attempts() const { return attempts_; }

private:
    unsigned fail_count_;
    int value_;
    unsigned attempts_ = 0;
};

// ── Success on first attempt ──────────────────────────────────────────────────

TEST(RetryTest, Success_FirstTry) {
    auto result = Utils::Retry::retry_on_error<int, TestError>(
        []() -> std::expected<int, TestError> { return 42; },
        3,
        retry_all);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

// ── Success after retry ───────────────────────────────────────────────────────

TEST(RetryTest, Success_AfterRetry) {
    FlakyCallable flaky(2, 99);

    auto result = Utils::Retry::retry_on_error<int, TestError>(
        [&flaky]() { return flaky(); },
        5,
        retry_all,
        1);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 99);
    EXPECT_EQ(flaky.attempts(), 3U);  // 2 failures + 1 success
}

// ── Exhaust retries ───────────────────────────────────────────────────────────

TEST(RetryTest, ExhaustRetries_ReturnsLastError) {
    FlakyCallable flaky(10, 1);  // requires 10 successes, but only 3 retries

    auto result = Utils::Retry::retry_on_error<int, TestError>(
        [&flaky]() { return flaky(); },
        3,
        retry_all,
        1);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TestError::TRANSIENT);
}

// ── Zero retries ──────────────────────────────────────────────────────────────

TEST(RetryTest, ZeroRetries_OneAttempt) {
    FlakyCallable flaky(1, 7);

    auto result = Utils::Retry::retry_on_error<int, TestError>(
        [&flaky]() { return flaky(); },
        0,
        retry_all);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(flaky.attempts(), 1U);
}

// ── Maximum retries ───────────────────────────────────────────────────────────

TEST(RetryTest, ExactlyEnoughRetries) {
    FlakyCallable flaky(3, 77);

    auto result = Utils::Retry::retry_on_error<int, TestError>(
        [&flaky]() { return flaky(); },
        3,
        retry_all,
        1);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 77);
    EXPECT_EQ(flaky.attempts(), 4U);  // 3 failures + 1 success
}

// ── Error predicate: retry only transient ─────────────────────────────────────

TEST(RetryTest, Predicate_RetriesTransientOnly) {
    auto result = Utils::Retry::retry_on_error<int, TestError>(
        []() -> std::expected<int, TestError> {
            return std::unexpected(TestError::PERMANENT);
        },
        3,
        [](const TestError& e) { return e == TestError::TRANSIENT; });

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TestError::PERMANENT);
}

TEST(RetryTest, Predicate_TransientErrors_AreRetried) {
    unsigned call_count = 0;

    auto result = Utils::Retry::retry_on_error<int, TestError>(
        [&call_count]() -> std::expected<int, TestError> {
            ++call_count;
            return call_count < 3
                       ? std::expected<int, TestError>(std::unexpected(TestError::TRANSIENT))
                       : std::expected<int, TestError>(42);
        },
        5,
        [](const TestError& e) { return e == TestError::TRANSIENT; },
        1);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
    EXPECT_EQ(call_count, 3U);
}

TEST(RetryTest, Predicate_AlwaysFail_SkipsRetry) {
    // Predicate returns false for *all* errors, so no retries occur.
    auto result = Utils::Retry::retry_on_error<int, TestError>(
        []() -> std::expected<int, TestError> {
            return std::unexpected(TestError::TRANSIENT);
        },
        5,
        [](const TestError&) { return false; });

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), TestError::TRANSIENT);
}

// ── actual_retries output parameter ───────────────────────────────────────────

TEST(RetryTest, ActualRetries_ZeroOnSuccess) {
    unsigned actual = 999;
    auto result = Utils::Retry::retry_on_error<int, TestError>(
        []() -> std::expected<int, TestError> { return 10; },
        3,
        retry_all,
        500,
        &actual);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 10);
    EXPECT_EQ(actual, 0U);
}

TEST(RetryTest, ActualRetries_CountedOnRetry) {
    FlakyCallable flaky(2, 55);
    unsigned actual = 999;

    auto result = Utils::Retry::retry_on_error<int, TestError>(
        [&flaky]() { return flaky(); },
        5,
        retry_all,
        1,  // minimal backoff to keep test fast
        &actual);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 55);
    // actual_retries should be 2 (the number of retries before success)
    EXPECT_EQ(actual, 2U);
}

TEST(RetryTest, ActualRetries_Exhausted) {
    FlakyCallable flaky(10, 1);
    unsigned actual = 999;

    auto result = Utils::Retry::retry_on_error<int, TestError>(
        [&flaky]() { return flaky(); },
        3,
        retry_all,
        1,
        &actual);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(actual, 3U);
}

TEST(RetryTest, ActualRetries_ZeroWhenPredicateSkips) {
    unsigned actual = 999;

    auto result = Utils::Retry::retry_on_error<int, TestError>(
        []() -> std::expected<int, TestError> {
            return std::unexpected(TestError::PERMANENT);
        },
        5,
        [](const TestError& e) { return e == TestError::TRANSIENT; },
        500,
        &actual);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(actual, 0U);
}

// ── Default backoff ───────────────────────────────────────────────────────────

TEST(RetryTest, DefaultBackoff_Is500ms) {
    // Verify the function uses the default backoff when not provided.
    // We use retry_all as the explicit predicate so backoff defaults to 500.
    FlakyCallable flaky(1, 1);

    auto start = std::chrono::steady_clock::now();
    auto result = Utils::Retry::retry_on_error<int, TestError>(
        [&flaky]() { return flaky(); },
        1,
        retry_all);  // implicit backoff = 500ms
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(result.has_value());
    // Should have slept ~500ms (allow generous tolerance for CI).
    EXPECT_GE(elapsed, std::chrono::milliseconds(400));
}
