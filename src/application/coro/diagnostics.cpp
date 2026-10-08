//
// app — coroutine diagnostic handlers (implementation).
//

#include "diagnostics.h"

#include <string>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "application/diagnostics.h"
#include "domain/dns/record_kind.h"

namespace app {

coro::Task<Diagnostics::DnsResolveOutcome> dns_resolve(ResolverPort& resolver, std::string host,
                                                       std::string type_text) {
    Diagnostics::DnsResolveOutcome outcome{
        .host = std::move(host), .type_text = std::move(type_text), .lookup = std::nullopt};

    const auto type = magic_enum::enum_cast<RecordKind>(outcome.type_text, magic_enum::case_insensitive);
    if (!type.has_value()) {
        co_return outcome;  // lookup stays nullopt — unknown record type
    }

    outcome.lookup = co_await resolver.resolve(outcome.host, *type);
    co_return outcome;
}

}  // namespace app
