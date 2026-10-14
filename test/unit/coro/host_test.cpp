//
// Coroutine runtime — the host extension surface (coro/host.hpp).
//
// The contracts the plugin host relies on: a CancelObserver fires exactly once
// on scope cancellation — inline during construction when the scope is already
// cancelled — and a GroupToken outlives its group without dangling.
//

#include "coro/host.hpp"

#include <coroutine>
#include <string>

#include <gtest/gtest.h>

#include "coro/coro.h"

namespace {

TEST(HostSurface, ObserverFiresOnCancel) {
    coro::CancelScope scope;
    int fired = 0;
    {
        const coro::CancelObserver observer{scope, &fired, [](void* owner) noexcept { ++*static_cast<int*>(owner); }};
        EXPECT_TRUE(observer.armed());
        EXPECT_EQ(fired, 0);
        scope.cancel();
        EXPECT_EQ(fired, 1);
        EXPECT_FALSE(observer.armed());
    }
    // Destruction after cancellation must not touch the scope again.
}

TEST(HostSurface, ObserverOnCancelledScopeFiresDuringConstruction) {
    coro::CancelScope scope;
    scope.cancel();
    int fired = 0;
    const coro::CancelObserver observer{scope, &fired, [](void* owner) noexcept { ++*static_cast<int*>(owner); }};
    EXPECT_EQ(fired, 1);
    EXPECT_FALSE(observer.armed());
}

TEST(HostSurface, ObserverFiresOnceAcrossRepeatedCancels) {
    coro::CancelScope scope;
    int fired = 0;
    const coro::CancelObserver observer{scope, &fired, [](void* owner) noexcept { ++*static_cast<int*>(owner); }};
    scope.cancel();
    scope.cancel();
    EXPECT_EQ(fired, 1);
}

TEST(HostSurface, DisarmedObserverIsNotNotified) {
    coro::CancelScope scope;
    int fired = 0;
    coro::CancelObserver observer{scope, &fired, [](void* owner) noexcept { ++*static_cast<int*>(owner); }};
    observer.disarm();
    EXPECT_FALSE(observer.armed());
    scope.cancel();
    EXPECT_EQ(fired, 0);
}

TEST(HostSurface, DestroyedObserverIsNotNotified) {
    coro::CancelScope scope;
    int fired = 0;
    {
        const coro::CancelObserver observer{scope, &fired, [](void* owner) noexcept { ++*static_cast<int*>(owner); }};
    }
    scope.cancel();
    EXPECT_EQ(fired, 0);
}

TEST(HostSurface, GroupTokenOutlivesItsGroup) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::GroupToken token{};
    EXPECT_FALSE(token);
    EXPECT_EQ(token.get(), nullptr);
    bool inside_ok = false;

    coro::run(loop, [&]() -> coro::Task<void> {
        co_await coro::supervisor_group([&](coro::TaskGroup& group) -> coro::Task<void> {
            token = coro::group_token(group);
            inside_ok = token.get() == &group;
            co_return;
        });
    }());

    EXPECT_TRUE(inside_ok);
    EXPECT_EQ(token.get(), nullptr);
}

}  // namespace
