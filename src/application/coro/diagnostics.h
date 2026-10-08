//
// app — coroutine diagnostic handlers.
//

#ifndef YADDNSC_APPLICATION_CORO_DIAGNOSTICS_H
#define YADDNSC_APPLICATION_CORO_DIAGNOSTICS_H

#include <string>

#include "application/coro/ports.h"
#include "application/diagnostics.h"
#include "infrastructure/coro/task.hpp"

namespace app {

/// Coroutine `dns resolve`: match the record type string case-insensitively
/// (legacy behaviour for direct invocations; the CLI parser already restricts
/// --type to A/AAAA/TXT) and look the name up through the resolver port.
///
/// A nullopt `lookup` means `type_text` was not a known record kind; the
/// presenter prints the "unknown record type" error. Failure: the lookup's
/// DnsErrorInfo value; a defect (allocation) propagates.
[[nodiscard]] coro::Task<Diagnostics::DnsResolveOutcome> dns_resolve(ResolverPort& resolver, std::string host,
                                                                     std::string type_text);

}  // namespace app

#endif  // YADDNSC_APPLICATION_CORO_DIAGNOSTICS_H
