#include "multicast.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <netinet/in.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

#include "domain/network/address_family.h"
#include "infrastructure/network/transport/socket_ops.h"

namespace net {

enum class IoError;

namespace {

void set_int_option(int fd, int level, int option, int value) {
    if (auto result = detail::set_socket_option(fd, level, option, &value, sizeof(value)); !result) {
        SPDLOG_DEBUG("Multicast socket option {} failed", option);
    }
}

std::expected<void, std::error_code> membership(int fd, const domain::InetAddress& group,
                                                const MulticastInterface& interface, bool join) noexcept {
    std::expected<void, IoError> result;
    if (const auto* v6 = group.as_v6(); v6 != nullptr) {
        ipv6_mreq request{};
        std::memcpy(&request.ipv6mr_multiaddr, v6->data(), sizeof(request.ipv6mr_multiaddr));
        request.ipv6mr_interface = interface.index;
        result = detail::set_socket_option(fd, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, &request,
                                           sizeof(request));
    } else {
        ip_mreq request{};
        std::memcpy(&request.imr_multiaddr, group.as_v4()->data(), sizeof(request.imr_multiaddr));
        std::memcpy(&request.imr_interface, interface.ipv4.data(), sizeof(request.imr_interface));
        result = detail::set_socket_option(fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &request,
                                           sizeof(request));
    }
    if (!result) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return {};
}

}  // namespace

MulticastMembership::MulticastMembership(int fd, const MulticastOptions& options) noexcept
    : fd_(fd), group_(options.group), interface_(options.interface) {}

MulticastMembership::~MulticastMembership() noexcept {
    leave();
}

MulticastMembership::MulticastMembership(MulticastMembership&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), group_(other.group_), interface_(other.interface_) {}

MulticastMembership& MulticastMembership::operator=(MulticastMembership&& other) noexcept {
    if (this != &other) {
        leave();
        fd_ = std::exchange(other.fd_, -1);
        group_ = other.group_;
        interface_ = other.interface_;
    }
    return *this;
}

void MulticastMembership::leave() noexcept {
    if (fd_ >= 0) {
        [[maybe_unused]] const auto result = membership(fd_, group_, interface_, false);
        fd_ = -1;
    }
}

std::expected<MulticastMembership, std::error_code> configure_multicast(int fd, const MulticastOptions& options) {
    const bool ipv6 = options.group.get_family() == domain::AddressFamily::IPV6;
    const auto index = options.interface.index;
#ifdef SO_REUSEPORT
    set_int_option(fd, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
    if (!options.interface_name.empty()) {
#if defined(SO_BINDTODEVICE)
        [[maybe_unused]] const auto bound = detail::bind_to_interface(fd, options.interface_name);
#elif defined(IP_BOUND_IF)
        if (index > 0) {
            const int level = ipv6 ? IPPROTO_IPV6 : IPPROTO_IP;
            const int option = ipv6 ? IPV6_BOUND_IF : IP_BOUND_IF;
            [[maybe_unused]] const auto bound = detail::set_socket_option(fd, level, option, &index, sizeof(index));
        }
#endif
        // FreeBSD has neither binding option; retain best-effort behavior.
    }
    if (ipv6) {
        set_int_option(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1);
        if (index > 0) {
            [[maybe_unused]] const auto selected =
                detail::set_socket_option(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &index, sizeof(index));
        }
        set_int_option(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, options.hops);
    } else {
        if (index > 0) {
#if defined(__linux__)
            ip_mreqn output{};
            output.imr_ifindex = static_cast<int>(index);
#else
            in_addr output{};
            std::memcpy(&output, options.interface.ipv4.data(), sizeof(output));
#endif
            [[maybe_unused]] const auto selected =
                detail::set_socket_option(fd, IPPROTO_IP, IP_MULTICAST_IF, &output, sizeof(output));
        }
        set_int_option(fd, IPPROTO_IP, IP_MULTICAST_TTL, options.hops);
    }
    if (auto joined = membership(fd, options.group, options.interface, true); !joined) {
        return std::unexpected(joined.error());
    }
    return MulticastMembership{fd, options};
}

}  // namespace net
