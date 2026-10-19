// mDNS receive loop, separated from multicast setup so filtering/backoff can
// be exercised on isolated loopback sockets.
#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_MDNS_RECEIVE_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_MDNS_RECEIVE_H

#include <string>

#include "coro/task.hpp"
#include "infrastructure/ip_source/result.h"
#include "infrastructure/network/transport/udp_socket.h"

namespace domain {
enum class RecordKind;
}

namespace ipsource::detail {

/// Collect the first matching mDNS answer from a borrowed nonblocking socket.
/// Ownership: socket and hostname must outlive the task. No multicast setup here.
/// Failure: receive failures become IpSourceError; invalid traffic is discarded
/// with periodic cancellable backoff. The caller's scope bounds the window.
/// Cancellation: propagates coro::Cancelled. Thread safety: loop-thread only.
[[nodiscard]] coro::Task<Result> receive_mdns(net::UdpSocket& socket, const std::string& hostname,
                                              domain::RecordKind type);

}  // namespace ipsource::detail

#endif  // YADDNSC_INFRASTRUCTURE_IP_SOURCE_MDNS_RECEIVE_H
