//
// app — coroutine application ports.
//
// The coroutine update stack talks to the outside world through these three
// interfaces. They are the coroutine counterparts of src/application/ports/:
// the operation is a coroutine, cancellation is the awaiting scope (there is no
// token parameter), and every outcome is a value.
//
// Concrete adapters live in infrastructure (dns::Dispatcher, the coroutine IP
// sources, plugin::DriverGateway), which is why the ports name only domain
// types.
//

#ifndef YADDNSC_APPLICATION_CORO_PORTS_H
#define YADDNSC_APPLICATION_CORO_PORTS_H

#include <string>
#include <vector>

#include <expected>

#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "domain/update/driver_update_command.h"
#include "infrastructure/coro/task.hpp"

namespace app {

/// ResolverPort — look up the current DNS records for a name.
///
/// Failure: DnsErrorInfo values; cancellation surfaces as DnsError::CANCELLED
/// (a definitive value), and the await is a scope checkpoint.
/// Thread safety: an implementation may hold session state; call from the loop
/// thread only.
class ResolverPort {
public:
    virtual ~ResolverPort() = default;

    [[nodiscard]] virtual coro::Task<std::expected<std::vector<std::string>, DnsErrorInfo>> resolve(
        std::string host, RecordKind type) = 0;
};

/// IpSourcePort — obtain local address candidates for a subdomain.
///
/// Failure: IpSourceError values (cancellation is IpSourceError::Code::CANCELLED).
/// An empty candidate list is a success.
class IpSourcePort {
public:
    virtual ~IpSourcePort() = default;

    [[nodiscard]] virtual coro::Task<std::expected<std::vector<InetAddress>, domain::IpSourceError>> resolve(
        const domain::SubdomainConfig& config) = 0;
};

/// GatewayPort — perform one driver update.
///
/// Failure: DriverError values. Cancellation: the await is an abandon point —
/// the driver cycle keeps its place on its lane and the result is
/// DriverError::CANCELLED.
class GatewayPort {
public:
    virtual ~GatewayPort() = default;

    [[nodiscard]] virtual coro::Task<std::expected<void, domain::DriverError>> update(
        std::string driver_name, domain::DriverUpdateCommand command) = 0;
};

}  // namespace app

#endif  // YADDNSC_APPLICATION_CORO_PORTS_H
