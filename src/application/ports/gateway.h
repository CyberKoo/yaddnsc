#ifndef YADDNSC_APPLICATION_PORTS_GATEWAY_H
#define YADDNSC_APPLICATION_PORTS_GATEWAY_H

#include "application/ports/driver_update.h"
#include "application/ports/driver_validation.h"
#include "coro/task.hpp"

namespace app {

/// The composed driver port: publishing updates and validating configuration.
///
/// The two halves are separate ports because the run path needs the update
/// side and the configuration sweep needs the validation side. A driver host
/// implements both, so the composition root keeps them together in one
/// object behind this composed port.
///
/// Failure: DriverError values. Cancellation propagates as `coro::Cancelled`:
/// the await is an abandon point. A driver cycle that already started runs to
/// completion; a cycle not yet started never enters the driver.
/// The port must outlive the task. Thread safety: loop-thread only.
class GatewayPort : public DriverUpdatePort, public DriverValidationPort {
public:
    ~GatewayPort() override = default;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_PORTS_GATEWAY_H
