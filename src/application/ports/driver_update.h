#ifndef YADDNSC_APPLICATION_PORTS_DRIVER_UPDATE_H
#define YADDNSC_APPLICATION_PORTS_DRIVER_UPDATE_H

#include <expected>
#include <string>

#include "coro/task.hpp"
#include "domain/error/error.h"
#include "domain/update/driver_update_command.h"

namespace app {

/// DriverUpdatePort — the application port that publishes one record.
///
/// Failure: DriverError values. Cancellation propagates as `coro::Cancelled`:
/// the await is an abandon point. A driver cycle that already started runs to
/// completion; a cycle not yet started never enters the driver.
/// The port must outlive the task. Thread safety: loop-thread only.
class DriverUpdatePort {
public:
    virtual ~DriverUpdatePort() = default;

    /// Publish one record through the named driver.
    [[nodiscard]] virtual coro::Task<std::expected<void, domain::DriverError>> update(
        std::string driver_name, domain::DriverUpdateCommand command) = 0;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_PORTS_DRIVER_UPDATE_H
