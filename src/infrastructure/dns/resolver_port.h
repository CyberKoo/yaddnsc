//
// dns — Dispatcher as an application ResolverPort.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_PORT_H
#define YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_PORT_H

#include <string>
#include <utility>
#include <vector>

#include <expected>

#include "application/ports/resolver.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "infrastructure/dns/dispatcher.h"

namespace dns {

/// DispatcherResolverPort — app::ResolverPort over a coroutine Dispatcher.
///
/// Ownership: borrows the dispatcher, which must outlive the port.
/// Thread safety: the dispatcher is loop-thread only, and so is this port.
class DispatcherResolverPort final : public app::ResolverPort {
public:
    explicit DispatcherResolverPort(Dispatcher& dispatcher) noexcept : dispatcher_(dispatcher) {}

    [[nodiscard]] coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> resolve(
        std::string host, domain::RecordKind type) override {
        co_return co_await dispatcher_.resolve(std::move(host), type);
    }

private:
    Dispatcher& dispatcher_;
};

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_RESOLVER_PORT_H
