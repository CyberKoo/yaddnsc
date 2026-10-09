//
// app — the cohesive bundles the coroutine update stack draws on.
//

#ifndef YADDNSC_APPLICATION_SERVICES_H
#define YADDNSC_APPLICATION_SERVICES_H

#include <functional>

#include "application/ports.h"
#include "application/ports/log.h"
#include "infrastructure/coro/fwd.h"

namespace app {

/// Services — what one update cycle needs. All references are non-owning and
/// must outlive the run.
struct Services {
    ResolverPort& resolver;
    IpSourcePort& ip_source;
    GatewayPort& gateway;
    const Logger& logger;
};

/// RuntimeServices — what the scheduler runner needs.
///
/// The driver gateway needs a TaskGroup to spawn its bridge coroutines into, and
/// that group only exists inside coro::run; `make_gateway` is therefore called
/// once, at the top of the run, with the runner's root group. The returned
/// reference must stay valid for the whole run (the composition root owns the
/// gateway it builds).
struct RuntimeServices {
    ResolverPort& resolver;
    IpSourcePort& ip_source;
    const Logger& logger;
    std::function<GatewayPort&(coro::TaskGroup& bridge_group)> make_gateway;
    /// Flush and stop the async log pipeline. The escalating second SIGINT calls
    /// this right before `_exit`, which skips the normal drain in main().
    std::function<void()> drain_logs;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_SERVICES_H
