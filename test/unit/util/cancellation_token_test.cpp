// Unit tests for util/cancellation_token.hpp — shared-owning, poll-based
// cancellation with reliable downward propagation.

#include "support/util/cancellation_token.hpp"

#include <chrono>
#include <stop_token>
#include <thread>

#include <gtest/gtest.h>
#include <poll.h>

using namespace std::chrono_literals;

TEST(CancellationTokenTest, DefaultTokenIsInert) {
    const Utils::CancellationToken token;
    EXPECT_EQ(token.native_handle(), -1);
    EXPECT_FALSE(static_cast<bool>(token));
    EXPECT_FALSE(token.is_triggered());
}

TEST(CancellationTokenTest, TriggerLatchesAndMakesFdReadable) {
    Utils::CancellationSource source;
    const auto token = source.token();

    source.trigger();

    EXPECT_TRUE(source.is_triggered());
    EXPECT_TRUE(token.is_triggered());
    pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
    ASSERT_EQ(::poll(&pfd, 1, 0), 1);
    EXPECT_TRUE(pfd.revents & POLLIN);
}

TEST(CancellationTokenTest, TriggerIsIdempotentAndSignalPersists) {
    Utils::CancellationSource source;
    const auto token = source.token();

    source.trigger();
    source.trigger();
    source.trigger();

    EXPECT_TRUE(token.is_triggered());
    // Cancellation notifications are never drained: every concurrent waiter
    // continues to observe the same terminal signal.
    pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 1);
}

TEST(CancellationTokenTest, TokenSurvivesSourceDestruction) {
    auto token = [] {
        const Utils::CancellationSource source;
        return source.token();
    }();

    ASSERT_TRUE(static_cast<bool>(token));
    pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 0);
    EXPECT_EQ(pfd.revents & (POLLHUP | POLLERR | POLLNVAL), 0);
}

TEST(CancellationTokenTest, TriggerWakesBlockedPollFromAnotherThread) {
    Utils::CancellationSource source;
    const auto token = source.token();

    std::jthread trigger_thread([src = source] {
        std::this_thread::sleep_for(50ms);
        src.trigger();
    });

    const auto start = std::chrono::steady_clock::now();
    pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
    ASSERT_EQ(::poll(&pfd, 1, 5000), 1);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    EXPECT_TRUE(token.is_triggered());
}

TEST(CancellationTokenTest, CopiesObserveTheSameState) {
    Utils::CancellationSource source;
    const auto first = source.token();
    const auto second = source.token();

    source.trigger();

    EXPECT_TRUE(first.is_triggered());
    EXPECT_TRUE(second.is_triggered());
}

TEST(CancellationTokenTest, ParentTriggerBroadcastsToChildAndGrandchild) {
    Utils::CancellationSource root;
    const auto child = root.derive();
    const auto grandchild = child.derive();

    root.trigger();

    EXPECT_TRUE(root.is_triggered());
    EXPECT_TRUE(child.is_triggered());
    EXPECT_TRUE(grandchild.is_triggered());
    for (const auto& token : {root.token(), child.token(), grandchild.token()}) {
        pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
        EXPECT_EQ(::poll(&pfd, 1, 0), 1);
    }
}

TEST(CancellationTokenTest, ChildTriggerDoesNotPropagateUpwardsOrToSiblings) {
    Utils::CancellationSource root;
    const auto child = root.derive();
    const auto sibling = root.derive();

    child.trigger();

    EXPECT_FALSE(root.is_triggered());
    EXPECT_TRUE(child.is_triggered());
    EXPECT_FALSE(sibling.is_triggered());
}

TEST(CancellationTokenTest, DerivingAfterParentTriggerReturnsTriggeredChild) {
    Utils::CancellationSource root;
    root.trigger();

    const auto child = root.token().derive_source();

    EXPECT_TRUE(child.is_triggered());
    EXPECT_TRUE(child.token().is_triggered());
    pollfd pfd{.fd = child.token().native_handle(), .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 1);
}

TEST(CancellationTokenTest, MultipleWaitersCannotConsumeCancellation) {
    Utils::CancellationSource source;
    const auto token = source.token();

    std::jthread first([token] {
        pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
        EXPECT_EQ(::poll(&pfd, 1, 5000), 1);
    });
    std::jthread second([token] {
        pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
        EXPECT_EQ(::poll(&pfd, 1, 5000), 1);
    });

    std::this_thread::sleep_for(50ms);
    source.trigger();
}

TEST(CancellationTokenTest, Bind_StopRequested_TriggersSourceAndToken) {
    Utils::CancellationSource source;
    const auto token = source.token();
    std::stop_source stop_source;
    const auto binding = source.bind(stop_source.get_token());

    stop_source.request_stop();

    EXPECT_TRUE(source.is_triggered());
    EXPECT_TRUE(token.is_triggered());
    // The stop-driven trigger lands on the same poll()-visible signal as a
    // direct trigger(), so every existing I/O wait path needs no special case.
    pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 1);
}

TEST(CancellationTokenTest, Bind_StopAlreadyRequested_TriggersSourceInline) {
    Utils::CancellationSource source;
    const auto token = source.token();
    std::stop_source stop_source;
    stop_source.request_stop();

    // Binding after the stop was requested must not open a cancellation
    // window: there is no later point at which the token could still be live.
    const auto binding = source.bind(stop_source.get_token());

    EXPECT_TRUE(source.is_triggered());
    EXPECT_TRUE(token.is_triggered());
}

TEST(CancellationTokenTest, Bind_StopRequested_ReachesDerivedSources) {
    Utils::CancellationSource root;
    const auto child = root.derive();
    const auto grandchild = child.derive();
    std::stop_source stop_source;
    const auto binding = root.bind(stop_source.get_token());

    stop_source.request_stop();

    // The binding triggers the root state, which broadcasts down the tree:
    // a stop request reaches poll()-based I/O at every level.
    for (const auto& token : {root.token(), child.token(), grandchild.token()}) {
        EXPECT_TRUE(token.is_triggered());
        pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
        EXPECT_EQ(::poll(&pfd, 1, 0), 1);
    }
}

TEST(CancellationTokenTest, Bind_BindingDestroyedBeforeStop_DoesNotTriggerSource) {
    Utils::CancellationSource source;
    const auto token = source.token();
    std::stop_source stop_source;
    {
        const auto binding = source.bind(stop_source.get_token());
    }

    stop_source.request_stop();

    // The binding owned the registration; destroying it unregisters.
    EXPECT_FALSE(source.is_triggered());
    EXPECT_FALSE(token.is_triggered());
    pollfd pfd{.fd = token.native_handle(), .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&pfd, 1, 0), 0);
}
