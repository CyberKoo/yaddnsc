#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "coro/host.hpp"

namespace {

struct DestructionProbe {
    std::vector<int>& trace;
    int id;

    DestructionProbe(std::vector<int>& events, int value) : trace(events), id(value) {}

    ~DestructionProbe() noexcept { trace.push_back(id); }
};

using ProbeOwner = std::unique_ptr<DestructionProbe>;

coro::Task<ProbeOwner> finish_after_body(coro::Nursery& nursery, const coro::NurseryToken& token,
                                         const ProbeOwner& resource, const ProbeOwner& factory,
                                         std::vector<int>& trace) {
    co_await coro::checkpoint();
    EXPECT_EQ(token.get(), nullptr);
    EXPECT_NE(resource, nullptr);
    EXPECT_NE(factory, nullptr);
    EXPECT_THROW(nursery.spawn([]() -> coro::Task<void> { co_return; }()), std::logic_error);
    trace.push_back(1);
    co_return std::make_unique<DestructionProbe>(trace, 2);
}

coro::Task<int> value() {
    co_return 7;
}

coro::Task<void> defect() {
    throw std::runtime_error("child defect");
    co_return;
}

TEST(CoroNursery, BodyReturn_DrainsBeforeReleasingPayloadFactoryAndResources) {
    std::vector<int> trace;
    trace.reserve(16);
    coro::Handle<ProbeOwner> retained;
    coro::NurseryToken token;
    coro::run(coro::with_nursery(std::make_unique<DestructionProbe>(trace, 4),
                                 [factory = std::make_unique<DestructionProbe>(trace, 3), &trace, &retained, &token](
                                     coro::Nursery& nursery, ProbeOwner& resource) -> coro::Task<void> {
                                     token = coro::nursery_token(nursery);
                                     retained =
                                         nursery.spawn(finish_after_body(nursery, token, resource, factory, trace));
                                     co_return;
                                 }));
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_FALSE(retained.valid());
    EXPECT_EQ(token.get(), nullptr);
    // Keeping an invalid handle must not keep its result payload alive.
    EXPECT_THROW(coro::run([](coro::Handle<ProbeOwner> handle) -> coro::Task<void> {
                     [[maybe_unused]] auto result = co_await handle;
                 }(retained)),
                 std::logic_error);
}

TEST(CoroNursery, ReadyHandle_ConsumesOnceAndInvalidatesCopies) {
    coro::run(coro::with_nursery([](coro::Nursery& nursery) -> coro::Task<void> {
        auto handle = nursery.spawn(value());
        auto copy = handle;
        co_await coro::checkpoint();
        co_await coro::checkpoint();  // Result is retired before testing the READY fast path.
        EXPECT_EQ(co_await handle, 7);
        EXPECT_FALSE(copy.valid());
        bool rejected = false;
        try {
            [[maybe_unused]] auto ignored = co_await copy;
        } catch (const std::logic_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }));
}

TEST(CoroNursery, Supervisor_ReportsUnobservedDiscardDefectAfterDrain) {
    bool reported = false;
    try {
        coro::run(coro::with_nursery(
            [](coro::Nursery& nursery) -> coro::Task<void> {
                nursery.spawn_discard(defect());
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR));
    } catch (const coro::GroupError& error) {
        reported = true;
        EXPECT_EQ(error.errors().size(), 1U);
        EXPECT_THROW(std::rethrow_exception(error.errors().front()), std::runtime_error);
    }
    EXPECT_TRUE(reported);
}

TEST(CoroNursery, FailFast_ClosesIngressAndPropagatesChildDefect) {
    coro::NurseryToken token;
    EXPECT_THROW(coro::run(coro::with_nursery([&token](coro::Nursery& nursery) -> coro::Task<void> {
                     token = coro::nursery_token(nursery);
                     nursery.spawn_discard(defect());
                     co_await coro::sleep_for(std::chrono::hours{1});
                 })),
                 std::runtime_error);
    EXPECT_EQ(token.get(), nullptr);
}

TEST(CoroNursery, SynchronousFactoryFailure_CancelsAndDrainsPartialSpawnBeforeCleanup) {
    std::vector<int> trace;
    trace.reserve(16);
    bool retired = false;
    EXPECT_THROW(
        coro::run(coro::with_nursery(std::make_unique<DestructionProbe>(trace, 4),
                                     [&trace, &retired](coro::Nursery& nursery, ProbeOwner&) -> coro::Task<void> {
                                         nursery.spawn([](bool& done, std::vector<int>& events) -> coro::Task<void> {
                                             struct Finished {
                                                 bool& value;
                                                 std::vector<int>& trace;
                                                 ~Finished() noexcept {
                                                     value = true;
                                                     trace.push_back(1);
                                                 }
                                             } finished{done, events};
                                             co_await coro::sleep_for(std::chrono::hours{1});
                                         }(retired, trace));
                                         throw std::runtime_error("factory defect");
                                     })),
        std::runtime_error);
    EXPECT_TRUE(retired);
    EXPECT_EQ(trace, (std::vector<int>{1, 4}));
}

}  // namespace
