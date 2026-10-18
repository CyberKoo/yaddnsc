#include "coro/detail/execution_record.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "coro/coro.h"
#include "coro/detail/access.h"
#include "coro/detail/context_awaitables.h"
#include "coro/detail/nursery_state.hpp"
#include "support/util/fd.hpp"

namespace {
static_assert(!std::is_move_constructible_v<coro::detail::ExecutionRecord>);
static_assert(!std::is_copy_constructible_v<coro::detail::ExecutionRecord>);
static_assert(!std::is_move_constructible_v<coro::detail::TaskAwaiter<int>>);
static_assert(!std::is_move_constructible_v<coro::detail::ChildEntry<int>>);

TEST(ExecutionRecord, RecordLayout_StaysWithinTheCostBudget) {
    // These are performance budgets, not ABI guarantees: the record and the
    // inline awaiter are embedded in every awaited level.
    EXPECT_LE(sizeof(coro::detail::ExecutionRecord), 120U);
    EXPECT_LE(sizeof(coro::detail::TaskAwaiter<int>), 168U);
    EXPECT_LE(sizeof(coro::detail::ChildEntry<int>), 208U);
    EXPECT_EQ(sizeof(coro::detail::WaitCore), 16U);
    EXPECT_EQ(sizeof(coro::detail::WaitOperation), 40U);
}

TEST(ExecutionRecord, SharedCompletionRoles_KeepSeparateContextsAndPhases) {
    struct Counts {
        int terminal = 0;
        int retired = 0;

        static void on_terminal(void* context, coro::detail::ExecutionRecord& record) noexcept {
            EXPECT_EQ(record.phase(), coro::detail::ExecutionPhase::TERMINAL);
            ++static_cast<Counts*>(context)->terminal;
        }

        static void on_retired(void* context, coro::detail::ExecutionRecord& record) noexcept {
            EXPECT_EQ(record.phase(), coro::detail::ExecutionPhase::RETIRED);
            ++static_cast<Counts*>(context)->retired;
        }
    } first, second;

    coro::detail::ExecutionRecord a, b;
    a.bind_completion<&Counts::on_terminal, &Counts::on_retired>(&first);
    b.bind_completion<&Counts::on_terminal, &Counts::on_retired>(&second);
    for (auto* record : {&a, &b}) {
        record->mark_active();
        record->mark_terminal();
        record->begin_reap();
        record->mark_retired();
        record->notify_retired();
    }
    EXPECT_EQ(first.terminal, 1);
    EXPECT_EQ(first.retired, 1);
    EXPECT_EQ(second.terminal, 1);
    EXPECT_EQ(second.retired, 1);
}

coro::Task<int> chain(int depth) {
    if (depth == 0)
        co_return 1;
    co_return 1 + co_await chain(depth - 1);
}

TEST(ExecutionRecord, DeepInlineChain_AddsNoEmptyRetirementBatches) {
    for (const int depth : {0, 1, 8, 64, 512}) {
        coro::Loop loop;

        struct Counts {
            int batches = 0;
            int resumes = 0;
            int polls = 0;
        } counts;

        loop.set_trace_sink(
            [](void* context, std::string_view message, const std::source_location&) noexcept {
                auto& observed = *static_cast<Counts*>(context);
                constexpr std::string_view PREFIX = "Drained ready queue, resumed ";
                if (message.starts_with(PREFIX)) {
                    ++observed.batches;
                    int resumed = 0;
                    [[maybe_unused]] const auto parsed =
                        std::from_chars(message.data() + PREFIX.size(), message.data() + message.size(), resumed);
                    observed.resumes += resumed;
                } else if (message.starts_with("poll() on ")) {
                    ++observed.polls;
                }
            },
            &counts);
        EXPECT_EQ(coro::run(loop, chain(depth)), depth + 1);
        EXPECT_EQ(counts.batches, 2 * depth + 1);
        EXPECT_EQ(counts.resumes, 2 * depth + 1);
        RecordProperty("batches_depth_" + std::to_string(depth), counts.batches);
        RecordProperty("resumes_depth_" + std::to_string(depth), counts.resumes);
        RecordProperty("polls_depth_" + std::to_string(depth), counts.polls);
    }
}

struct FrameProbe {
    std::vector<int>& trace;

    explicit FrameProbe(std::vector<int>& events) : trace(events) {}

    ~FrameProbe() noexcept { trace.push_back(2); }
};

coro::Task<int> framed_value(std::unique_ptr<FrameProbe> probe) {
    probe->trace.push_back(1);
    co_return 42;
}

TEST(ExecutionRecord, InlineFrameParameter_IsDestroyedOnceBeforeParentResumes) {
    std::vector<int> trace;
    trace.reserve(8);
    coro::run([&]() -> coro::Task<void> {
        EXPECT_EQ(co_await framed_value(std::make_unique<FrameProbe>(trace)), 42);
        trace.push_back(3);
    }());
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 3}));
}

TEST(ExecutionRecord, NurseryFrame_RetiresBeforeUnconsumedResultIsClaimed) {
    std::vector<int> trace;
    trace.reserve(8);
    coro::run(coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
        auto result = nursery.spawn(framed_value(std::make_unique<FrameProbe>(trace)));
        co_await coro::checkpoint();
        co_await coro::checkpoint();
        EXPECT_EQ(trace, (std::vector<int>{1, 2}));
        EXPECT_EQ(co_await result, 42);
        trace.push_back(3);
    }));
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 3}));
}

TEST(ExecutionRecord, ShieldedInlineChild_ParentCancellationDoesNotResumeEarly) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    coro::CancelScope* parent_scope = nullptr;
    bool shield_finished = false;
    bool parent_resumed = false;
    coro::run(
        loop, coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
            nursery.spawn_discard([](coro::CancelScope*& target) -> coro::Task<void> {
                while (target == nullptr)
                    co_await coro::checkpoint();
                target->cancel();
            }(parent_scope));
            const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
                parent_scope = &scope;
                const auto shield = co_await coro::non_cancellable([&](coro::CancelScope&) -> coro::Task<void> {
                    co_await coro::sleep_for(std::chrono::milliseconds{5});
                    EXPECT_FALSE(parent_resumed);
                    shield_finished = true;
                });
                EXPECT_TRUE(shield.completed);
                EXPECT_TRUE(shield_finished);
                parent_resumed = true;
            });
            EXPECT_TRUE(outcome.cancelled);
        }));
    EXPECT_TRUE(parent_resumed);
}

TEST(ExecutionRecord, DuplicateReadyInsertion_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            coro::Loop loop;
            coro::detail::ExecutionRecord record;
            coro::detail::LoopAccess::schedule(loop, record);
            coro::detail::LoopAccess::schedule(loop, record);
        },
        "");
}

TEST(ExecutionRecord, RetirementWhileStillQueuedForResumption_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            coro::Loop loop;
            coro::detail::ExecutionRecord record;
            auto task = []() -> coro::Task<void> { co_return; }();
            record.adopt(coro::detail::TaskAccess::release(task));
            record.mark_active();
            record.mark_terminal();
            // The ready token still borrows this frame, so retiring the record now
            // would destroy the frame under a queued resumption. That is the one
            // invariant the ready queue can no longer check by itself.
            coro::detail::LoopAccess::schedule(loop, record);
            record.begin_reap();
        },
        "");
}

TEST(ExecutionRecord, RootRetirementWithOrphanRegistryEntry_TerminatesImmediately) {
    EXPECT_DEATH(
        {
            coro::Loop loop;
            coro::detail::ExecutionRecord orphan;
            coro::run(loop, [&]() -> coro::Task<void> {
                coro::detail::LoopAccess::register_execution(loop, orphan);
                co_return;
            }());
        },
        "");
}

TEST(ExecutionRecord, LoopDestructionWithRegisteredExecution_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            auto loop = std::make_unique<coro::Loop>();
            coro::detail::ExecutionRecord orphan;
            coro::detail::LoopAccess::register_execution(*loop, orphan);
            loop.reset();
        },
        "");
}

TEST(ExecutionRecord, IllegalPhaseTransitions_TerminateInEveryBuild) {
    for (const int operation : {0, 1, 2, 3}) {
        EXPECT_DEATH(
            {
                coro::detail::ExecutionRecord record;
                if (operation == 0) {
                    record.mark_active();
                    record.mark_active();
                } else if (operation == 1) {
                    record.mark_terminal();
                } else if (operation == 2) {
                    record.begin_reap();
                } else {
                    record.mark_retired();
                }
            },
            "");
    }
}

TEST(ExecutionRecord, RecursiveRunFromInbox_TerminatesInsteadOfAdvancingNestedDispatch) {
    EXPECT_DEATH(
        {
            coro::Loop loop;
            loop.post([&loop] { loop.run(); });
            coro::run(loop, []() -> coro::Task<void> { co_return; }());
        },
        "");
}

TEST(ExecutionRecord, LoopDestroyedDuringInboxDispatch_TerminatesBeforeClosingResources) {
    EXPECT_DEATH(
        {
            auto loop = std::make_unique<coro::Loop>();
            loop->post([&loop] { loop.reset(); });
            coro::run(*loop, []() -> coro::Task<void> { co_return; }());
        },
        "");
}

struct AbortIfDestroyed {
    ~AbortIfDestroyed() noexcept { std::_Exit(0); }
};

TEST(ExecutionRecord, ActiveInlineAwaiterHost_RejectsDestructionBeforeChildRetirement) {
    EXPECT_DEATH(
        {
            coro::Loop loop;
            coro::CancelScope scope;
            coro::detail::ExecutionRecord parent;
            parent.bind_context(loop, &scope);
            parent.mark_active();
            coro::detail::PromiseBase promise;
            promise.exec = &parent;
            auto task = []() -> coro::Task<void> { co_return; }();
            coro::detail::TaskAwaiter<void> awaiter{coro::detail::TaskAccess::release(task)};
            coro::detail::TaskAccess::start_inline(awaiter, promise);
        },
        "");
}

TEST(ExecutionRecord, ActiveChildHost_RejectsDestructionBeforePayloadMembers) {
    EXPECT_DEATH(
        {
            auto entry = std::make_unique<coro::detail::ChildEntry<std::unique_ptr<AbortIfDestroyed>>>();
            entry->typed_result.write_value(std::make_unique<AbortIfDestroyed>());
            entry->record.mark_active();
            entry.reset();
        },
        "");
}

TEST(ExecutionRecord, DispatchException_TerminatesRatherThanUnwindingLiveFrames) {
    EXPECT_DEATH(
        {
            coro::Loop loop;
            loop.post([] { throw std::runtime_error("dispatch defect"); });
            [[maybe_unused]] auto result = coro::run(loop, chain(1));
        },
        "");
}

struct OrderedFrame {
    std::vector<int>& trace;
    int value;
    bool* destroyed;

    OrderedFrame(std::vector<int>& events, int id, bool* flag = nullptr) : trace(events), value(id), destroyed(flag) {}

    ~OrderedFrame() noexcept {
        trace.push_back(value);
        if (destroyed != nullptr)
            *destroyed = true;
    }
};

coro::Task<int> ordered_value(std::unique_ptr<OrderedFrame> probe) {
    co_return probe->value;
}

TEST(ExecutionRecord, SameBatchChildren_RetireAndPublishResultsInFifoOrder) {
    std::vector<int> retired, consumed;
    retired.reserve(8);
    coro::run(coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
        for (const int value : {1, 2, 3}) {
            nursery.spawn(ordered_value(std::make_unique<OrderedFrame>(retired, value)));
        }
        while (const auto result = co_await nursery.next<int>())
            consumed.push_back(result->value());
    }));
    EXPECT_EQ(retired, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(consumed, retired);
}

TEST(ExecutionRecord, SameBatchTerminalHandle_SuspendsUntilFrameDestruction) {
    std::vector<int> trace;
    trace.reserve(8);
    bool destroyed = false;
    coro::run(coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
        auto handle = nursery.spawn(ordered_value(std::make_unique<OrderedFrame>(trace, 2, &destroyed)));
        nursery.spawn_discard([](coro::Handle<int> result, bool& gone, std::vector<int>& events) -> coro::Task<void> {
            EXPECT_FALSE(gone);  // Producer terminated earlier in this ready batch.
            events.push_back(1);
            EXPECT_EQ(co_await result, 2);
            EXPECT_TRUE(gone);
            events.push_back(3);
        }(handle, destroyed, trace));
        co_return;
    }));
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 3}));
}

struct PostOnDestroy {
    coro::Loop& loop;
    bool& destroying;
    bool& delivered;

    PostOnDestroy(coro::Loop& owner, bool& active, bool& done) : loop(owner), destroying(active), delivered(done) {}

    ~PostOnDestroy() noexcept {
        destroying = true;
        loop.post([&active = destroying, &done = delivered] {
            EXPECT_FALSE(active);
            done = true;
        });
        EXPECT_FALSE(delivered);
        destroying = false;
    }
};

coro::Task<void> post_from_frame(std::unique_ptr<PostOnDestroy> probe) {
    EXPECT_NE(probe, nullptr);
    co_return;
}

TEST(ExecutionRecord, FrameDestructorPost_DoesNotDispatchInboxInsideRetirement) {
    coro::Loop loop;
    bool destroying = false, delivered = false;
    coro::run(loop, [&]() -> coro::Task<void> {
        co_await post_from_frame(std::make_unique<PostOnDestroy>(loop, destroying, delivered));
        EXPECT_TRUE(delivered);
    }());
    EXPECT_TRUE(delivered);
}

coro::Task<void> set_flag(bool& flag) {
    flag = true;
    co_return;
}

struct SpawnOnDestroy {
    coro::Nursery& nursery;
    bool& accepted;
    bool& rejected;
    bool& ran;

    SpawnOnDestroy(coro::Nursery& n, bool& a, bool& r, bool& done) : nursery(n), accepted(a), rejected(r), ran(done) {}

    ~SpawnOnDestroy() noexcept {
        try {
            nursery.spawn_discard(set_flag(ran));
            accepted = true;
            EXPECT_FALSE(ran);  // Reaping can enqueue, never resume inline.
        } catch (const std::logic_error&) {
            rejected = true;
        }
    }
};

coro::Task<void> spawn_from_frame(std::unique_ptr<SpawnOnDestroy> parameter) {
    EXPECT_NE(parameter, nullptr);
    co_return;
}

TEST(ExecutionRecord, FrameDestructorSpawn_OpenAdmissionAddsChildAndClosingAdmissionRejects) {
    for (const bool keep_body_open : {true, false}) {
        bool accepted = false, rejected = false, ran = false;
        coro::run(coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
            auto child =
                nursery.spawn(spawn_from_frame(std::make_unique<SpawnOnDestroy>(nursery, accepted, rejected, ran)));
            if (keep_body_open)
                co_await child;
        }));
        EXPECT_EQ(accepted, keep_body_open);
        EXPECT_EQ(rejected, !keep_body_open);
        EXPECT_EQ(ran, keep_body_open);
    }
}

/// A frame parameter that owns a timer and an fd registration. Its destructor
/// runs inside the retirement transaction, where unregistering is explicitly
/// allowed: idempotent, allocation-free and no-throw (design 3.4).
class RegistrationOnDestroy {
public:
    RegistrationOnDestroy(coro::Loop& loop, int fd, int& dispatches) noexcept : loop_(&loop) {
        entry_.fd = fd;
        entry_.events = POLLIN;
        // The callback touches only storage this test owns, so a registration
        // that wrongly outlived the frame still cannot read the freed probe.
        entry_.fn = [](void* context, short /*revents*/) noexcept { ++*static_cast<int*>(context); };
        entry_.context = &dispatches;
        coro::detail::LoopAccess::add_fd(loop, entry_);
        coro::detail::LoopAccess::add_timer(loop, timer_, loop.now() + std::chrono::hours{1}, nullptr, nullptr);
    }

    RegistrationOnDestroy(const RegistrationOnDestroy&) = delete;
    RegistrationOnDestroy& operator=(const RegistrationOnDestroy&) = delete;
    RegistrationOnDestroy(RegistrationOnDestroy&&) = delete;
    RegistrationOnDestroy& operator=(RegistrationOnDestroy&&) = delete;

    ~RegistrationOnDestroy() noexcept {
        // Twice each: removal is documented as idempotent, so the second call
        // must neither unlink again nor throw.
        for (int pass = 0; pass < 2; ++pass) {
            coro::detail::LoopAccess::remove_timer(*loop_, timer_);
            coro::detail::LoopAccess::remove_fd(*loop_, entry_);
        }
    }

private:
    coro::Loop* loop_ = nullptr;
    coro::detail::TimerNode timer_{};
    coro::detail::FdEntry entry_{};
};

coro::Task<void> registered_child(std::unique_ptr<RegistrationOnDestroy> parameter) {
    EXPECT_NE(parameter, nullptr);
    co_return;
}

TEST(ExecutionRecord, FrameDestructorUnregistersItsTimerAndItsFd) {
    coro::ManualClock clock;
    std::vector<std::string> lines;
    coro::Loop loop{clock};
    loop.set_trace_sink(
        [](void* context, std::string_view message, const std::source_location&) noexcept {
            static_cast<std::vector<std::string>*>(context)->emplace_back(message);
        },
        &lines);

    auto [read_end, write_end] = Utils::make_pipe();
    ASSERT_TRUE(read_end);
    ASSERT_TRUE(write_end);
    const char byte = 'x';

    const coro::TimePoint start = clock.now();
    int dispatches = 0;
    auto body = [&](coro::Nursery& nursery) -> coro::Task<void> {
        nursery.spawn_discard(
            registered_child(std::make_unique<RegistrationOnDestroy>(loop, read_end.get(), dispatches)));
        co_await coro::checkpoint();
        // The child's frame was destroyed at the previous batch end, so both
        // registrations are gone before the descriptor may become ready.
        co_await coro::checkpoint();
        EXPECT_EQ(::write(write_end.get(), &byte, 1), 1);
        const std::error_code ec = co_await coro::wait_readable(read_end.get());
        EXPECT_FALSE(ec);
    };
    coro::run(loop, coro::with_nursery(body));

    EXPECT_EQ(dispatches, 0) << "an fd registration outlived the frame that owned it";
    // A surviving timer would be this heap's minimum, and a manual clock only
    // advances by jumping to a timer deadline: a lost disarmed timer shows up as
    // a one-hour jump here rather than as a crash.
    EXPECT_LT(clock.now() - start, std::chrono::seconds{1});
    const auto count_lines = [&lines](std::string_view needle) {
        return std::ranges::count_if(
            lines, [needle](const std::string& line) { return line.find(needle) != std::string::npos; });
    };
    EXPECT_EQ(count_lines("Disarmed timer"), 1);
    EXPECT_GE(count_lines("Unregistered fd"), 2);  // the probe's registration and the root's own wait
}

/// Cancels another scope from a frame destructor: the reaper runs this
/// synchronous bookkeeping hook while the loop's queues and the target scope are
/// still live (design 3.4).
struct CancelOnDestroy {
    coro::CancelScope** target = nullptr;

    ~CancelOnDestroy() noexcept {
        if (target != nullptr && *target != nullptr) {
            (*target)->cancel();
        }
    }
};

coro::Task<void> cancel_from_frame(std::unique_ptr<CancelOnDestroy> parameter) {
    EXPECT_NE(parameter, nullptr);
    // Parks once, so the frame retires one batch after the target scope exists
    // and while the scope's waiter is still parked.
    co_await coro::checkpoint();
    co_return;
}

TEST(ExecutionRecord, FrameDestructorCancellingAParkedWait_ResumesItOnceWithoutLosingTheWake) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const coro::TimePoint start = clock.now();
    coro::CancelScope* parked_scope = nullptr;
    bool wait_saw_cancellation = false;
    bool body_completed = true;

    auto body = [&](coro::Nursery& nursery) -> coro::Task<void> {
        auto probe = std::make_unique<CancelOnDestroy>();
        probe->target = &parked_scope;
        nursery.spawn_discard(cancel_from_frame(std::move(probe)));
        const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
            parked_scope = &scope;
            try {
                co_await coro::sleep_for(std::chrono::hours{1});
            } catch (const coro::Cancelled&) {
                wait_saw_cancellation = true;
                throw;
            }
        });
        body_completed = outcome.completed;
        EXPECT_TRUE(outcome.cancelled);
    };
    coro::run(loop, coro::with_nursery(body));

    EXPECT_TRUE(wait_saw_cancellation) << "the parked wait must observe the destructor's cancellation";
    EXPECT_FALSE(body_completed);
    // A lost wakeup would leave the sleep to expire, which this manual clock
    // reports as a one-hour jump instead of a silent pass.
    EXPECT_LT(clock.now() - start, std::chrono::seconds{1});
}
}  // namespace
