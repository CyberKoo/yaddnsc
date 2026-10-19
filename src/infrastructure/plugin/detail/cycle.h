#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_CYCLE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_CYCLE_H

#include <atomic>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "coro/host.hpp"
#include "coro/notification.hpp"
#include "coro/scope_outcome.hpp"  // IWYU pragma: keep — compiled standalone as a header check, so the include must stay
#include "coro/task.hpp"
#include "domain/error/error.h"

namespace coro {
class CancelScope;
}

namespace plugin {
struct BridgeCall;

/// Shared cycle delivery state. Only cancelled is concurrently accessed;
/// worker_error is worker-owned until joined completion. All remaining fields
/// and notification operations are loop-only. Owners survive subscriber exit
/// until real worker/HTTP drain and unobserved-defect reporting finish.
struct CallState {
    std::atomic<bool> cancelled{false};
    coro::NurseryToken lifetime;
    std::vector<std::weak_ptr<BridgeCall>> calls;
    coro::Notification owner_changed;
    coro::Notification cancel_changed;
    coro::Notification result_ready;
    std::expected<void, domain::DriverError> result;
    std::exception_ptr error;
    std::exception_ptr worker_error;
    coro::CancelScope* worker_scope = nullptr;
    bool subscriber_active = false;
    bool error_observed = false;
    bool plugin_cancelled = false;
    bool worker_done = false;

    void record_worker_error(std::exception_ptr defect) noexcept {
        if (!worker_error) {
            worker_error = std::move(defect);
        }
    }

    /// Loop-only, allocation-free cancellation hook.
    void request_cancel() noexcept;
};

/// Root-owned boundary around a pre-built shielded body. Retains cancellation
/// observation through real drain, then reports defects not consumed by the
/// subscriber. Both tasks own their frames before root-group submission.
[[nodiscard]] coro::Task<void> own_cycle(std::shared_ptr<CallState> state,
                                         coro::Task<coro::ScopeOutcome<void>> shielded);

/// Root-group admission. Closed ingress throws; a spawn failure records and
/// notifies the failure before rethrowing. Loop-thread only.
void start_cycle(const coro::NurseryToken& group, const std::shared_ptr<CallState>& state, coro::Task<void> owner);

/// Loop-only subscription. Borrows scope/state through its lifetime; arm before
/// starting an owner. Waiting parks the caller itself, not an intermediate task,
/// so cancellation still wins at the caller's result-consumption checkpoint.
class CycleSubscription {
public:
    CycleSubscription(CallState& state, coro::CancelScope& scope) noexcept;
    ~CycleSubscription() noexcept;
    CycleSubscription(const CycleSubscription&) = delete;
    CycleSubscription& operator=(const CycleSubscription&) = delete;

    [[nodiscard]] auto wait() const noexcept { return state_.result_ready.wait(); }

    /// Call after wait succeeds. Defects propagate; plugin-only cancellation is
    /// an error value. fqdn selects update diagnostics, nullopt validation.
    [[nodiscard]] std::expected<void, domain::DriverError> take_result(
        std::string_view driver_name, std::optional<std::string_view> fqdn = std::nullopt) const;

private:
    CallState& state_;
    coro::CancelScope& scope_;
    coro::CancelObserver cancellation_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_CYCLE_H
