//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATER_H
#define YADDNSC_CORE_UPDATER_H

#include "application/ports/dns_resolver.h"
#include "application/ports/driver_gateway.h"
#include "application/ports/ip_source.h"
#include "application/ports/log.h"

struct UpdateTask;

/// Updater — application workflow that processes a single UpdateTask.
///
/// Depends only on application ports: DnsResolverPort (verify current record),
/// IpSourcePort (local address candidates), DriverGateway (perform the update)
/// and Logger (diagnostics). No concrete resolver, IP source, driver, or HTTP
/// types cross this boundary.
///
/// @note process() is thread-safe and may be called concurrently from multiple
///       pool threads: the Updater itself owns no mutable state and every port
///       implementation is required to be thread-safe.
class Updater {
public:
    /// Construct with the workflow's ports (all non-owning; owned by Manager::Impl).
    Updater(const DnsResolverPort &dns_resolver, const IpSourcePort &ip_source,
            const DriverGateway &driver_gateway, const Logger &logger);

    /// Execute a single update task.
    ///
    /// Steps:
    ///   1. Resolve local address candidates via the IP source port and pick
    ///      one with domain::select_address (AAAA link-local/ULA policy).
    ///   2. Unless force_update, compare against the current DNS record and
    ///      skip when unchanged. A failed lookup never blocks the update.
    ///   3. Hand the update to the driver gateway and log the outcome.
    ///
    /// Every expected failure arrives as an error value and is logged in
    /// place; the noexcept catch-all below remains only as a defence against
    /// unexpected exceptions (e.g. std::bad_alloc), mirroring the legacy
    /// "log and swallow" boundary.
    ///
    /// @note Never throws — all errors and outcomes are logged internally.
    void process(const UpdateTask &task) const noexcept;

private:
    const DnsResolverPort &dns_resolver_;
    const IpSourcePort &ip_source_;
    const DriverGateway &driver_gateway_;
    const Logger &logger_;
};

#endif // YADDNSC_CORE_UPDATER_H
