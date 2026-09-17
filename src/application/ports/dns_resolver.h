//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_APPLICATION_PORTS_DNS_RESOLVER_H
#define YADDNSC_APPLICATION_PORTS_DNS_RESOLVER_H

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "dns/dns_error_info.h"
#include "record_kind.h"

/// DnsResolverPort — application port for DNS resolution.
///
/// The application sees only this port: no raw DNS packets, sockets, or
/// concrete classic/DoH/DoT resolver types cross the boundary.
///
/// Result contract:
///   - success carries the record list (non-empty in practice today: a
///     NOERROR response with zero records is reported as DnsError::NODATA —
///     Phase 0 behaviour, kept);
///   - every failure is a structured DnsErrorInfo value, never an exception;
///   - workflows treat every failure uniformly as "cannot verify the current
///     record → proceed with the update" (see the README behaviour table).
///
/// Cancellation is bound inside the infrastructure implementation at
/// construction time (Utils::CancellationToken); the port itself stays
/// free of I/O cancellation types.
class DnsResolverPort {
public:
    virtual ~DnsResolverPort() = default;

    /// Resolve `host` for the given record type.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, DnsErrorInfo>
    resolve(std::string_view host, RecordKind type) const = 0;
};

#endif // YADDNSC_APPLICATION_PORTS_DNS_RESOLVER_H
