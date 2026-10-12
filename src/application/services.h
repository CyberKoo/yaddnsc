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
    /// Per-record deadline for the pre-start configuration validation sweep.
    std::chrono::milliseconds driver_validation_budget{5000};
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

/// RuntimeServices — what the main loop needs.
///
/// The driver gateway needs a TaskGroup to spawn its bridge coroutines into,
/// and that group only exists inside coro::run; `make_gateway` is therefore
/// called once, at the top of the run, with the main loop's root group, and the
/// main loop owns the returned gateway for the whole run.
struct RuntimeServices {
    ResolverPort& resolver;
    IpSourcePort& ip_source;
    const LoggerPort& logger;
    std::function<std::unique_ptr<GatewayPort>(coro::TaskGroup& bridge_group)> make_gateway;
    /// Latched process stop request; safe to query before launching work.
    /// Both callbacks are required and their owner must outlive coro::run.
    std::function<bool()> stop_requested;
    /// Cancellable wait for that same request, including an earlier arrival.
    /// The process adapter owns signal installation and forced termination.
    std::function<coro::Task<void>()> wait_for_stop;
    UpdatePolicy policy{};
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_SERVICES_H
