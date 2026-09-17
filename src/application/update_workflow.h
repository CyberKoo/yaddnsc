//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_UPDATE_WORKFLOW_H
#define YADDNSC_APPLICATION_UPDATE_WORKFLOW_H

#include <expected>

#include "application/ports/dns_resolver.h"
#include "application/ports/driver_gateway.h"
#include "application/ports/ip_source.h"
#include "application/ports/log.h"

#include "domain/error/error.h"
#include "domain/update/update_decision.h"

namespace domain {
    struct UpdateTask;
}

/// Outcome of one executed update cycle.
struct UpdateResult {
    domain::UpdateDecision decision;  ///< What the cycle decided (and did)
};

/// UpdateWorkflow result: the decision taken, or the failure that ended the
/// cycle early. Every expected failure is an UpdateError value — the legacy
/// catch-all remains only as a mapping for unexpected exceptions, never as
/// the sole error path.
using UpdateOutcome = std::expected<UpdateResult, domain::UpdateError>;

/// UpdateWorkflow — the application use case for one DDNS update cycle
/// (refactor/phase-3-scheduling-and-workflow.md §3.4).
///
/// Steps (address resolution and the DNS read are private steps of the
/// workflow, not sibling services):
///   1. Resolve local address candidates via the IP source port and pick one
///      with domain::select_address (AAAA link-local/ULA policy).
///   2. Unless force_update, read the current DNS records; a failed lookup is
///      logged and mapped to an empty record list (the update still happens).
///   3. Evaluate domain::decide_update (only the FIRST record is compared).
///   4. Hand the update to the driver gateway when the decision requires it.
///
/// Depends only on application ports: no concrete resolver, IP source,
/// driver, HTTP client or thread pool crosses this boundary, and spdlog is
/// never included — business events go through the Logger facade with their
/// call-site source location.
///
/// @note run() is thread-safe and may be called concurrently from multiple
///       pool threads: the workflow owns no mutable state and every port
///       implementation is required to be thread-safe.
class UpdateWorkflow {
public:
    /// Construct with the workflow's ports (all non-owning; owned by the
    /// composition root — currently still Manager::Impl).
    UpdateWorkflow(const DnsResolverPort &dns_resolver, const IpSourcePort &ip_source,
                   const DriverGateway &driver_gateway, const Logger &logger);

    /// Execute one update cycle for `task`.
    ///
    /// Every expected failure is logged in place with the legacy wording and
    /// returned as an UpdateError; the caller (TaskExecutor) may discard the
    /// result — the schedule has already been advanced by the queue.
    UpdateOutcome run(const domain::UpdateTask &task) const;

private:
    const DnsResolverPort &dns_resolver_;
    const IpSourcePort &ip_source_;
    const DriverGateway &driver_gateway_;
    const Logger &logger_;
};

#endif // YADDNSC_APPLICATION_UPDATE_WORKFLOW_H
