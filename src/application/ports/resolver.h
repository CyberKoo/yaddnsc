#ifndef YADDNSC_APPLICATION_PORTS_RESOLVER_H
#define YADDNSC_APPLICATION_PORTS_RESOLVER_H

#include <string>
#include <vector>

#include <expected>

#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "coro/task.hpp"

namespace app {

/// Look up current DNS records for updates and one-shot diagnostics.
///
/// Failure: DnsErrorInfo values; cancellation propagates as `coro::Cancelled`.
/// The await is a scope checkpoint. The port must outlive the task.
/// Thread safety: an implementation may hold session state; loop-thread only.
class ResolverPort {
public:
    virtual ~ResolverPort() = default;

    [[nodiscard]] virtual coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> resolve(
        std::string host, domain::RecordKind type) = 0;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_PORTS_RESOLVER_H
