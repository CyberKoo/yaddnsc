//
// Unit tests for util/cancellation_token.hpp — Utils::CancellationToken /
// Utils::CancellationSource (shared-owning, poll-based cancellation).
//
// Verifies:
//   - Default-constructed token is inert.
//   - Source/token start untriggered; trigger() latches on both.
//   - Trigger makes the token fd readable (poll wakes with POLLIN).
//   - drain() clears the pipe but the latched flag survives.
//   - Tokens keep the pipe alive after the source is destroyed.
//   - trigger() is idempotent and thread-safe (cross-thread wakeup).
//   - Copies of a token all observe the same state.
// =============================================================================

#include "support/util/cancellation_token.hpp"

#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <poll.h>

using namespace std::chrono_literals;

// ── Default construction ─────────────────────────────────────────────────────

TEST(CancellationTokenTest, DefaultToken_IsInert) {
    const Utils::CancellationToken token;
    EXPECT_EQ(token.native_handle(), -1);
    EXPECT_FALSE(static_cast<bool>(token));
    EXPECT_FALSE(token.is_triggered());
    // drain() on an inert token must be a no-op (no crash).
    token.drain();
}

// ── Fresh source is untriggered ──────────────────────────────────────────────

TEST(CancellationTokenTest, FreshSource_IsUntriggered) {
    const Utils::CancellationSource source;
    const auto token = source.token();

    EXPECT_TRUE(static_cast<bool>(token));
    EXPECT_GE(token.native_handle(), 0);
    EXPECT_FALSE(source.is_triggered());
    EXPECT_FALSE(token.is_triggered());

    // Pipe must not be readable yet.
    pollfd pfd{token.native_handle(), POLLIN, 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 0);
}

// ── Trigger latches on source and token ──────────────────────────────────────

TEST(CancellationTokenTest, Trigger_LatchesOnSourceAndToken) {
    Utils::CancellationSource source;
    const auto token = source.token();

    source.trigger();

    EXPECT_TRUE(source.is_triggered());
    EXPECT_TRUE(token.is_triggered());
}

TEST(CancellationTokenTest, Trigger_MakesFdReadable) {
    Utils::CancellationSource source;
    const auto token = source.token();

    source.trigger();

    pollfd pfd{token.native_handle(), POLLIN, 0};
    ASSERT_EQ(::poll(&pfd, 1, 100), 1);
    EXPECT_TRUE(pfd.revents & POLLIN);
}

// ── drain() clears the pipe, flag stays latched ──────────────────────────────

TEST(CancellationTokenTest, Drain_ClearsPipeButKeepsFlag) {
    Utils::CancellationSource source;
    const auto token = source.token();

    source.trigger();
    token.drain();

    // Pipe drained: no longer readable.
    pollfd pfd{token.native_handle(), POLLIN, 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 0);

    // Latched flag still reports triggered.
    EXPECT_TRUE(token.is_triggered());
    EXPECT_TRUE(source.is_triggered());
}

// ── Token outlives the source (shared ownership) ─────────────────────────────

TEST(CancellationTokenTest, Token_SurvivesSourceDestruction) {
    auto token = [] {
        const Utils::CancellationSource source;
        return source.token();
    }();

    // The fd stays valid: poll must NOT report HUP/ERR.
    ASSERT_TRUE(static_cast<bool>(token));
    pollfd pfd{token.native_handle(), POLLIN, 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 0);
    EXPECT_EQ(pfd.revents & (POLLHUP | POLLERR | POLLNVAL), 0);
}

// ── Idempotency ──────────────────────────────────────────────────────────────

TEST(CancellationTokenTest, Trigger_IsIdempotent) {
    Utils::CancellationSource source;
    const auto token = source.token();

    source.trigger();
    source.trigger();
    source.trigger();

    EXPECT_TRUE(token.is_triggered());
    token.drain();
    EXPECT_TRUE(token.is_triggered());
}

// ── Cross-thread wakeup ──────────────────────────────────────────────────────

TEST(CancellationTokenTest, Trigger_WakesBlockedPollFromAnotherThread) {
    Utils::CancellationSource source;
    const auto token = source.token();

    std::jthread trigger_thread([src = source] {
        std::this_thread::sleep_for(50ms);
        src.trigger();
    });

    const auto start = std::chrono::steady_clock::now();
    pollfd pfd{token.native_handle(), POLLIN, 0};
    const int r = ::poll(&pfd, 1, 5000);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_EQ(r, 1);
    EXPECT_TRUE(pfd.revents & POLLIN);
    EXPECT_LT(elapsed, 2s);
}

// ── Copies share state ───────────────────────────────────────────────────────

TEST(CancellationTokenTest, CopiesObserveSameState) {
    Utils::CancellationSource source;
    const auto token1 = source.token();
    const auto token2 = source.token();

    source.trigger();

    EXPECT_TRUE(token1.is_triggered());
    EXPECT_TRUE(token2.is_triggered());
}

TEST(CancellationTokenTest, TriggerAfterSourceCopy_LatchesBoth) {
    const Utils::CancellationSource source1;
    const Utils::CancellationSource source2(source1);  // same underlying state
    const auto token = source2.token();

    source1.trigger();

    EXPECT_TRUE(source2.is_triggered());
    EXPECT_TRUE(token.is_triggered());
}
