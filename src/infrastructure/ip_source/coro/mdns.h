//
// ip_source — coroutine mDNS source (offload transition).
//
// Native coroutine mDNS (multicast socket + UdpSocket framing) is not ported
// yet; this source runs the legacy synchronous implementation on the offload
// pool. That is deliberate transition debt for stage 3, not the intended final
// shape.
//

#ifndef YADDNSC_IP_SOURCE_CORO_MDNS_H
#define YADDNSC_IP_SOURCE_CORO_MDNS_H

#include <string>

#include "domain/dns/record_kind.h"
#include "infrastructure/ip_source/coro/source.h"

namespace ipsource {

/// MdnsIpSource — discover a LAN device's address via mDNS.
///
/// Transition: wraps the legacy synchronous source through coro::offload, so a
/// cancelled scope abandons the wait while the worker finishes on its own.
class MdnsIpSource final : public CoroIpSource {
public:
    MdnsIpSource(std::string hostname, RecordKind type, std::string interface);

    [[nodiscard]] coro::Task<Result> resolve() override;

private:
    std::string hostname_;
    RecordKind type_;
    std::string interface_;
};

}  // namespace ipsource

#endif  // YADDNSC_IP_SOURCE_CORO_MDNS_H
