//
// app — the cohesive bundles the coroutine update stack draws on.
//

#ifndef YADDNSC_APPLICATION_SERVICES_H
#define YADDNSC_APPLICATION_SERVICES_H

#include <chrono>
#include <functional>
#include <memory>

#include "application/ports/gateway.h"
#include "application/ports/ip_source.h"
#include "application/ports/log.h"
#include "application/ports/resolver.h"
#include "coro/fwd.h"

namespace app {

/// Stage budgets assembled by composition; cancellation scopes enforce them.
struct UpdatePolicy {
    std::chrono::milliseconds update_budget{30000};
    std::chrono::milliseconds dns_read_budget{4000};
    std::chrono::milliseconds ip_source_budget{10000};
};

/// Services — what one update cycle needs. All references are non-owning and
/// must outlive the run.
struct Services {
    ResolverPort& resolver;
    IpSourcePort& ip_source;
    GatewayPort& gateway;
    const LoggerPort& logger;
    UpdatePolicy policy{};
};

/// Stop-signal counts observed before (or after) the event loop's watch; see
/// RuntimeServices::startup_signals.
struct StartupSignalCounts {
    int sigint{0};
    int sigterm{0};
};

/// RuntimeServices — what the run root needs.
///
/// The driver gateway needs a TaskGroup to spawn its bridge coroutines into,
/// and that group only exists inside coro::run; `make_gateway` is therefore
/// called once, at the top of the run, with the runner's root group, and the
/// run root owns the returned gateway for the whole run.
struct RuntimeServices {
    ResolverPort& resolver;
    IpSourcePort& ip_source;
    const LoggerPort& logger;
    std::function<std::unique_ptr<GatewayPort>(coro::TaskGroup& bridge_group)> make_gateway;
    /// Immediately terminate with the supplied status. Must not wait for I/O
    /// or return; composition supplies the process adapter.
    std::function<void(int)> force_exit;
    /// Signals counted before the loop started (the composition root installs a
    /// counting handler for the startup phase). The run root folds them into
    /// its shutdown state, so a startup Ctrl-C is a graceful exit, not a bare
    /// kill. Empty means no startup bookkeeping (tests).
    std::function<StartupSignalCounts()> startup_signals;
    UpdatePolicy policy{};
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_SERVICES_H
