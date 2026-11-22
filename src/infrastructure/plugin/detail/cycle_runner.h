#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_CYCLE_RUNNER_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_CYCLE_RUNNER_H

#include <memory>
#include <optional>
#include <string_view>

#include "coro/host.hpp"
#include "coro/notification.hpp"
#include "infrastructure/plugin/detail/driver_session.h"

namespace coro {
class CancelScope;
}

namespace plugin {

/// Owns the delivery lifecycle of one driver session. Construct directly in
/// the caller's coroutine frame so result consumption preserves the caller's
/// cancellation and scheduling semantics without an additional Task layer.
class CycleRunner {
public:
    CycleRunner(coro::CancelScope& caller_scope, std::unique_ptr<DriverSession> session, coro::NurseryToken nursery,
                std::string_view driver_name, std::optional<std::string_view> fqdn = std::nullopt);
    ~CycleRunner();

    CycleRunner(const CycleRunner&) = delete;
    CycleRunner& operator=(const CycleRunner&) = delete;
    CycleRunner(CycleRunner&&) = delete;
    CycleRunner& operator=(CycleRunner&&) = delete;

    /// One live wait for result publication. Await as
    /// `co_await runner.result_ready().wait()` inside the caller's frame.
    [[nodiscard]] coro::Notification& result_ready() noexcept;
    [[nodiscard]] DriverSessionResult take_result();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_CYCLE_RUNNER_H
