#include "infrastructure/plugin/detail/cycle_runner.h"

#include <chrono>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "coro/loop.h"
#include "coro/notification.hpp"
#include "coro/run.hpp"
#include "coro/scope.hpp"
#include "coro/with_nursery.hpp"
#include "domain/error/error.h"
#include "infrastructure/plugin/detail/driver_session.h"
#include "mocks/allocation_fault.h"

namespace {
using namespace std::chrono_literals;

thread_local int allocations_before_failure = -1;

void allocation_hook(std::size_t /*size*/) {
    if (allocations_before_failure == 0) {
        allocations_before_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
}

class FailNextAllocation {
public:
    explicit FailNextAllocation(const int count = 0) noexcept { allocations_before_failure = count; }

    FailNextAllocation(const FailNextAllocation&) = delete;
    FailNextAllocation& operator=(const FailNextAllocation&) = delete;

    ~FailNextAllocation() noexcept { allocations_before_failure = -1; }
};

[[maybe_unused]] const bool allocation_hook_armed = [] {
    test::set_allocation_fault_hook(&allocation_hook);
    return true;
}();

struct SessionState {
    enum class Mode { SUCCESS, PLUGIN_CANCEL, DEFECT, WAIT_FOR_CANCEL, WAIT_FOR_CANCEL_DEFECT };

    Mode mode{Mode::SUCCESS};
    coro::Notification cancel_seen;
    int run_calls{0};
    int cancel_calls{0};
    int drain_calls{0};
};

class FakeSession final : public plugin::DriverSession {
public:
    explicit FakeSession(std::shared_ptr<SessionState> state) : state_(std::move(state)) {}

    [[nodiscard]] coro::Task<plugin::DriverSessionOutcome> run() override {
        ++state_->run_calls;
        if (state_->mode == SessionState::Mode::DEFECT) {
            throw std::runtime_error("session defect");
        }
        if (state_->mode == SessionState::Mode::WAIT_FOR_CANCEL ||
            state_->mode == SessionState::Mode::WAIT_FOR_CANCEL_DEFECT) {
            co_await state_->cancel_seen.wait();
        }
        if (state_->mode == SessionState::Mode::WAIT_FOR_CANCEL_DEFECT) {
            throw std::runtime_error("abandoned session defect");
        }
        co_return plugin::DriverSessionOutcome{{}, state_->mode == SessionState::Mode::PLUGIN_CANCEL};
    }

    void request_cancel() noexcept override {
        ++state_->cancel_calls;
        state_->cancel_seen.notify();
    }

    [[nodiscard]] coro::Task<void> drain() override {
        ++state_->drain_calls;
        co_return;
    }

private:
    std::shared_ptr<SessionState> state_;
};

std::unique_ptr<plugin::DriverSession> make_session(const std::shared_ptr<SessionState>& state) {
    return std::make_unique<FakeSession>(state);
}

coro::Task<plugin::DriverSessionResult> run_cycle(const std::shared_ptr<SessionState>& state, coro::Nursery& root,
                                                  std::string_view driver_name,
                                                  std::optional<std::string_view> fqdn = std::nullopt) {
    auto& scope = co_await coro::current_scope();
    plugin::CycleRunner runner{scope, make_session(state), coro::nursery_token(root), driver_name, fqdn};
    co_await runner.result_ready().wait();
    co_return runner.take_result();
}

TEST(CycleRunnerTest, ResultIsPublishedOnlyAfterDrain) {
    const auto state = std::make_shared<SessionState>();
    coro::Loop loop;
    coro::run(loop, coro::with_nursery([&](coro::Nursery& root) -> coro::Task<void> {
                  const auto result = co_await run_cycle(state, root, "fake", "www.example.com");
                  EXPECT_TRUE(result.has_value());
                  EXPECT_EQ(state->drain_calls, 1);
              }));
    EXPECT_EQ(state->run_calls, 1);
}

TEST(CycleRunnerTest, PluginCancellationBecomesDriverError) {
    const auto state = std::make_shared<SessionState>();
    state->mode = SessionState::Mode::PLUGIN_CANCEL;
    coro::Loop loop;
    coro::run(loop, coro::with_nursery([&](coro::Nursery& root) -> coro::Task<void> {
                  const auto result = co_await run_cycle(state, root, "fake", "www.example.com");
                  EXPECT_FALSE(result.has_value());
                  EXPECT_EQ(result.error().code, domain::DriverError::Code::UPDATE_FAILED);
                  EXPECT_NE(result.error().message.find("reported cancellation"), std::string::npos);
              }));
    EXPECT_EQ(state->drain_calls, 1);
}

TEST(CycleRunnerTest, ActiveCallerReceivesDefectAfterDrain) {
    const auto state = std::make_shared<SessionState>();
    state->mode = SessionState::Mode::DEFECT;
    coro::Loop loop;
    coro::run(loop, coro::with_nursery([&](coro::Nursery& root) -> coro::Task<void> {
                  try {
                      [[maybe_unused]] const auto result = co_await run_cycle(state, root, "fake");
                      ADD_FAILURE() << "the session defect should escape";
                  } catch (const std::runtime_error& error) {
                      EXPECT_STREQ(error.what(), "session defect");
                  }
                  EXPECT_EQ(state->drain_calls, 1);
              }));
}

TEST(CycleRunnerTest, CallerCancellationStillRunsCancelAndDrain) {
    const auto state = std::make_shared<SessionState>();
    state->mode = SessionState::Mode::WAIT_FOR_CANCEL;
    coro::Loop loop;
    coro::run(loop, coro::with_nursery([&](coro::Nursery& root) -> coro::Task<void> {
                  const auto outcome = co_await coro::with_timeout(5ms, [&] { return run_cycle(state, root, "fake"); });
                  EXPECT_TRUE(outcome.timed_out);
              }));
    EXPECT_EQ(state->run_calls, 1);
    EXPECT_EQ(state->cancel_calls, 1);
    EXPECT_EQ(state->drain_calls, 1);
}

TEST(CycleRunnerTest, AbandonedCallerLeavesDefectForRootBoundary) {
    const auto state = std::make_shared<SessionState>();
    state->mode = SessionState::Mode::WAIT_FOR_CANCEL_DEFECT;
    coro::Loop loop;
    bool observed = false;
    try {
        coro::run(loop, coro::with_nursery([&](coro::Nursery& root) -> coro::Task<void> {
                      const auto outcome =
                          co_await coro::with_timeout(5ms, [&] { return run_cycle(state, root, "fake"); });
                      EXPECT_TRUE(outcome.timed_out);
                  }));
    } catch (const std::runtime_error& error) {
        observed = true;
        EXPECT_STREQ(error.what(), "abandoned session defect");
    }
    EXPECT_TRUE(observed);
    EXPECT_EQ(state->cancel_calls, 1);
    EXPECT_EQ(state->drain_calls, 1);
}

TEST(CycleRunnerTest, PreProtocolAllocationFailureStillDrainsExactlyOnce) {
    const auto state = std::make_shared<SessionState>();
    coro::Loop loop;
    bool caught = false;
    try {
        coro::run(loop, coro::with_nursery([&](coro::Nursery& root) -> coro::Task<void> {
                      auto& scope = co_await coro::current_scope();
                      plugin::CycleRunner runner{scope, make_session(state), coro::nursery_token(root), "fake"};
                      const FailNextAllocation failure{2};
                      co_await runner.result_ready().wait();
                      [[maybe_unused]] const auto result = runner.take_result();
                  }));
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    EXPECT_TRUE(caught);
    EXPECT_EQ(state->drain_calls, 1);
}

}  // namespace
