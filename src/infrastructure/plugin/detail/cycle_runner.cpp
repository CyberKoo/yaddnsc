#include "cycle_runner.h"

#include <atomic>
#include <cassert>
#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/notification.hpp"
#include "coro/nursery.hpp"
#include "coro/scope.hpp"
#include "coro/with_nursery.hpp"
#include "domain/error/error.h"
#include "support/fmt.hpp"

namespace plugin {
namespace {

enum class CyclePhase {
    CREATED,
    RUNNING,
    COMPLETING,
    CANCELLING,
    DRAINING,
    PUBLISHING,
    AWAITING_SUBSCRIBER_RELEASE,
    DONE,
};

struct CycleControl {
    explicit CycleControl(std::unique_ptr<DriverSession> driver_session) : session(std::move(driver_session)) {}

    std::unique_ptr<DriverSession> session;
    CyclePhase phase{CyclePhase::CREATED};
    std::atomic<bool> cancelled{false};
    coro::Notification owner_changed;
    coro::Notification cancel_changed;
    coro::Notification result_ready;
    std::optional<DriverSessionResult> result;
    std::exception_ptr error;
    bool subscriber_active{false};
    bool error_observed{false};
    bool plugin_cancelled{false};
    bool session_done{false};
    bool drain_attempted{false};

    void request_cancel() noexcept {
        cancelled.store(true, std::memory_order_release);
        owner_changed.notify();
        cancel_changed.notify();
    }

    void record_error(std::exception_ptr defect) noexcept {
        if (!error) {
            error = std::move(defect);
        }
    }
};

using Control = std::shared_ptr<CycleControl>;

void request_cancel_hook(void* context) noexcept {
    static_cast<CycleControl*>(context)->request_cancel();
}

void transition(CycleControl& state, const CyclePhase expected, const CyclePhase next) {
    assert(state.phase == expected && "invalid driver cycle transition");
    if (state.phase != expected) {
        throw std::logic_error("invalid driver cycle transition");
    }
    state.phase = next;
}

coro::Task<void> watch_cancellation(Control state) {
    while (!state->session_done) {
        if (state->cancelled.load(std::memory_order_acquire)) {
            state->session->request_cancel();
            co_return;
        }
        co_await state->cancel_changed.wait();
    }
}

struct SessionDoneGuard {
    Control state;

    ~SessionDoneGuard() noexcept {
        state->session_done = true;
        state->cancel_changed.notify();
    }
};

coro::Task<void> drain_session(Control state) {
    if (state->drain_attempted) {
        co_return;
    }
    state->drain_attempted = true;
    try {
        co_await state->session->drain();
    } catch (const coro::Cancelled&) {
        state->plugin_cancelled = true;
        state->request_cancel();
    } catch (...) {
        state->record_error(std::current_exception());
    }
}

coro::Task<void> run_protocol(Control state) {
    transition(*state, CyclePhase::CREATED, CyclePhase::RUNNING);
    co_await coro::with_nursery(
        [&state](coro::Nursery& watcher_group) -> coro::Task<void> {
            const SessionDoneGuard done_guard{state};
            watcher_group.spawn_discard(watch_cancellation(state));

            try {
                auto outcome = co_await state->session->run();
                state->plugin_cancelled = outcome.plugin_cancelled;
                state->result.emplace(std::move(outcome.result));
            } catch (const coro::Cancelled&) {
                state->plugin_cancelled = true;
                state->request_cancel();
            } catch (...) {
                state->record_error(std::current_exception());
            }

            co_await drain_session(state);

            transition(
                *state, CyclePhase::RUNNING,
                state->cancelled.load(std::memory_order_acquire) ? CyclePhase::CANCELLING : CyclePhase::COMPLETING);
            if (state->phase == CyclePhase::CANCELLING) {
                transition(*state, CyclePhase::CANCELLING, CyclePhase::DRAINING);
            } else {
                transition(*state, CyclePhase::COMPLETING, CyclePhase::DRAINING);
            }
        },
        coro::NurseryPolicy::SUPERVISOR);
}

coro::Task<void> drive_cycle(Control state) {
    try {
        co_await run_protocol(state);
    } catch (const coro::Cancelled&) {
        state->plugin_cancelled = true;
        state->request_cancel();
    } catch (...) {
        state->record_error(std::current_exception());
    }

    co_await drain_session(state);
    state->phase = CyclePhase::PUBLISHING;
    state->result_ready.notify();
    state->phase = CyclePhase::AWAITING_SUBSCRIBER_RELEASE;
    while (state->subscriber_active) {
        co_await state->owner_changed.wait();
    }
    state->phase = CyclePhase::DONE;
}

coro::Task<void> own_cycle(Control state) {
    auto& parent = co_await coro::current_scope();
    const coro::CancelObserver cancellation{parent, state.get(), &request_cancel_hook};
    try {
        co_await coro::non_cancellable([state](coro::CancelScope&) mutable { return drive_cycle(state); });
    } catch (const coro::Cancelled&) {
        state->plugin_cancelled = true;
        state->request_cancel();
    } catch (...) {
        state->record_error(std::current_exception());
    }
    if (state->error && !state->error_observed) {
        std::rethrow_exception(state->error);
    }
}

class CycleSubscription {
public:
    CycleSubscription(Control state, coro::CancelScope& scope) noexcept
        : state_(std::move(state)), scope_(scope), cancellation_(scope, state_.get(), &request_cancel_hook) {
        state_->subscriber_active = true;
    }

    ~CycleSubscription() noexcept {
        state_->subscriber_active = false;
        state_->owner_changed.notify();
    }

    CycleSubscription(const CycleSubscription&) = delete;
    CycleSubscription& operator=(const CycleSubscription&) = delete;

    [[nodiscard]] DriverSessionResult take_result(std::string_view driver_name, std::optional<std::string_view> fqdn) {
        scope_.throw_if_cancelled();
        if (state_->error) {
            state_->error_observed = true;
            std::rethrow_exception(state_->error);
        }
        if (!state_->plugin_cancelled) {
            if (!state_->result.has_value()) {
                throw std::logic_error("driver cycle published without a result");
            }
            return std::move(*state_->result);
        }

        scope_.throw_if_cancelled();
        if (fqdn) {
            SPDLOG_WARN(R"(Driver '{}' reported cancellation for "{}"; the cycle fails and retries on schedule)",
                        driver_name, *fqdn);
            return std::unexpected(
                domain::DriverError{domain::DriverError::Code::UPDATE_FAILED,
                                    fmt::format("Driver '{}' reported cancellation for {}", driver_name, *fqdn), 0});
        }
        return std::unexpected(domain::DriverError{
            domain::DriverError::Code::UNKNOWN,
            fmt::format("Driver '{}' reported cancellation during configuration validation", driver_name), 0});
    }

private:
    Control state_;
    coro::CancelScope& scope_;
    coro::CancelObserver cancellation_;
};

void start_cycle(const coro::NurseryToken& nursery, const Control& state) {
    auto* group = nursery.get();
    if (group == nullptr) {
        throw std::logic_error("plugin cycle requires a live root nursery");
    }
    try {
        group->spawn_discard(own_cycle(state));
    } catch (...) {
        state->record_error(std::current_exception());
        state->result_ready.notify();
        throw;
    }
}

}  // namespace

struct CycleRunner::Impl {
    Control state;
    std::unique_ptr<CycleSubscription> subscription;
    std::string driver_name;
    std::optional<std::string> fqdn;
};

CycleRunner::CycleRunner(coro::CancelScope& caller_scope, std::unique_ptr<DriverSession> session,
                         coro::NurseryToken nursery, const std::string_view driver_name,
                         const std::optional<std::string_view> fqdn)
    : impl_(std::make_unique<Impl>()) {
    caller_scope.throw_if_cancelled();
    impl_->state = std::make_shared<CycleControl>(std::move(session));
    impl_->driver_name = driver_name;
    impl_->fqdn = fqdn ? std::optional<std::string>(*fqdn) : std::nullopt;
    impl_->subscription = std::make_unique<CycleSubscription>(impl_->state, caller_scope);
    start_cycle(nursery, impl_->state);
}

CycleRunner::~CycleRunner() = default;

coro::Notification& CycleRunner::result_ready() noexcept {
    return impl_->state->result_ready;
}

DriverSessionResult CycleRunner::take_result() {
    const auto fqdn = impl_->fqdn ? std::optional<std::string_view>(*impl_->fqdn) : std::nullopt;
    return impl_->subscription->take_result(impl_->driver_name, fqdn);
}

}  // namespace plugin
