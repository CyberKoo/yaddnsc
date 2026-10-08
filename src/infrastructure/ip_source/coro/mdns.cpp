//
// ip_source — coroutine mDNS source (implementation).
//

#include "mdns.h"

#include <utility>

#include "infrastructure/coro/offload.hpp"
#include "infrastructure/ip_source/mdns.h"
#include "support/util/cancellation_token.hpp"

namespace ipsource {

MdnsIpSource::MdnsIpSource(std::string hostname, RecordKind type, std::string interface)
    : hostname_(std::move(hostname)), type_(type), interface_(std::move(interface)) {}

coro::Task<Result> MdnsIpSource::resolve() {
    auto outcome = co_await coro::offload([this]() -> Result {
        ::MdnsIpSource legacy{hostname_, type_, interface_};
        return legacy.resolve(Utils::CancellationToken{});
    });
    if (!outcome.has_value()) {
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::CANCELLED, "mDNS lookup cancelled"});
    }
    co_return std::move(*outcome);
}

}  // namespace ipsource
