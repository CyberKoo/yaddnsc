#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_DRIVER_SESSION_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_DRIVER_SESSION_H

#include <expected>

#include "coro/task.hpp"
#include "domain/error/error.h"

namespace plugin {

using DriverSessionResult = std::expected<void, domain::DriverError>;

struct DriverSessionOutcome {
    DriverSessionResult result;
    bool plugin_cancelled{false};
};

/// One update or validation protocol execution. The CycleRunner calls run and
/// drain once, on the loop thread. request_cancel is idempotent and is also
/// called on the loop thread by the runner's cancellation watcher.
class DriverSession {
public:
    virtual ~DriverSession() = default;

    [[nodiscard]] virtual coro::Task<DriverSessionOutcome> run() = 0;
    virtual void request_cancel() noexcept = 0;
    [[nodiscard]] virtual coro::Task<void> drain() = 0;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_DRIVER_SESSION_H
