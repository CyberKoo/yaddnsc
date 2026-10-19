#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_MULTICAST_H
#define YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_MULTICAST_H

#include <expected>
#include <string_view>
#include <system_error>

#include "domain/network/inet_address.h"

namespace net {

/// Owned metadata acquired before socket setup, off-loop when required.
struct MulticastInterface {
    unsigned int index = 0;
    domain::Inet4Address ipv4{};
};

/// Protocol intent; the adapter chooses native option layouts. interface_name
/// is borrowed only during configure_multicast, never by the membership owner.
struct MulticastOptions {
    domain::InetAddress group;
    std::string_view interface_name;
    MulticastInterface interface;
    int hops = 1;
};

/// Owns a group join, not the descriptor. The fd must outlive the membership.
/// Destruction leaves the group best-effort. Move-only; loop-thread use.
class MulticastMembership {
public:
    ~MulticastMembership() noexcept;
    MulticastMembership(MulticastMembership&& other) noexcept;
    MulticastMembership& operator=(MulticastMembership&& other) noexcept;
    MulticastMembership(const MulticastMembership&) = delete;
    MulticastMembership& operator=(const MulticastMembership&) = delete;

private:
    friend std::expected<MulticastMembership, std::error_code> configure_multicast(int, const MulticastOptions&);
    MulticastMembership(int fd, const MulticastOptions& options) noexcept;
    void leave() noexcept;

    int fd_;
    domain::InetAddress group_;
    MulticastInterface interface_;
};

/// Best-effort coexistence, interface binding and hop-limit options, followed
/// by a required group join. Captures join errno in the returned error. Socket
/// options do not wait for network I/O; allocation/logging failures may throw.
[[nodiscard]] std::expected<MulticastMembership, std::error_code> configure_multicast(int fd,
                                                                                      const MulticastOptions& options);

}  // namespace net

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TRANSPORT_MULTICAST_H
