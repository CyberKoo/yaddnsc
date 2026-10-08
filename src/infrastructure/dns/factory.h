//
// dns — build a coroutine dispatcher from resolver settings.
//
// The coroutine counterpart of DnsResolverFactory: it maps each configured
// server to a classic / DoT / DoH backend and applies the configured strategy.
//

#ifndef YADDNSC_DNS_FACTORY_H
#define YADDNSC_DNS_FACTORY_H

#include <memory>
#include <vector>

#include "domain/config/dns_config.h"
#include "domain/config/runtime_config.h"
#include "infrastructure/dns/dispatcher.h"

namespace net {
class TlsContext;
}  // namespace net

namespace dns {

/// Build a Dispatcher from normalized resolver settings.
///
/// @param settings     Normalized resolver settings (server list + strategy).
/// @param bootstrap    Bootstrap DNS servers handed to the DoH/DoT backends.
/// @param tls_context  Pre-built trust context for TLS endpoints (DoT/DoH);
///                     pass null to leave them fail-closed.
/// @throws std::invalid_argument when the server list is empty or a server
///         address is malformed / uses an unknown schema (configuration
///         validation should have rejected both first).
[[nodiscard]] std::unique_ptr<Dispatcher> make_dispatcher(const domain::ResolverSettings& settings,
                                                          std::vector<Config::DnsServer> bootstrap,
                                                          std::shared_ptr<const net::TlsContext> tls_context);

}  // namespace dns

#endif  // YADDNSC_DNS_FACTORY_H
