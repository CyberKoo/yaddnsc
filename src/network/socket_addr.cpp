//
// Created by Kotarou on 2026/7/6.
//
#include "network/socket_addr.h"

#include <algorithm>
#include <cstdint>

#include "network/inet_address.h"

#include <arpa/inet.h>
#include <netinet/in.h>

// ===========================================================================
//  Factory methods
// ===========================================================================

std::optional<SocketAddr> SocketAddr::from_inet(const InetAddress& addr, std::uint16_t port) noexcept
{
  SocketAddr result;

  addr.visit([&]<typename T>(const T& concrete) {
    if constexpr (std::is_same_v<T, Inet4Address>) {
      auto& sin = *reinterpret_cast<sockaddr_in*>(&result.storage_);
      sin.sin_family = AF_INET;
      sin.sin_port = htons(port);
      std::ranges::copy_n(concrete.data(), 4, reinterpret_cast<std::uint8_t*>(&sin.sin_addr));
      result.len_ = sizeof(sin);
    } else if constexpr (std::is_same_v<T, Inet6Address>) {
      auto& sin6 = *reinterpret_cast<sockaddr_in6*>(&result.storage_);
      sin6.sin6_family = AF_INET6;
      sin6.sin6_port = htons(port);
      sin6.sin6_flowinfo = 0;
      std::ranges::copy_n(concrete.data(), 16, reinterpret_cast<std::uint8_t*>(&sin6.sin6_addr));
      sin6.sin6_scope_id = concrete.get_scope_id();
      result.len_ = sizeof(sin6);
    }
  });

  if (result.storage_.ss_family == AF_UNSPEC) {
    return std::nullopt;
  }
  return result;
}

SocketAddr SocketAddr::from_raw(const sockaddr* addr, socklen_t len) noexcept
{
  SocketAddr result;
  if (addr && len > 0 && static_cast<size_t>(len) <= sizeof(result.storage_)) {
    std::ranges::copy_n(reinterpret_cast<const std::uint8_t*>(addr), static_cast<std::ptrdiff_t>(len),
                        reinterpret_cast<std::uint8_t*>(&result.storage_));
    result.len_ = len;
  }
  return result;
}

// ===========================================================================
//  Accessors
// ===========================================================================

std::uint16_t SocketAddr::port() const noexcept
{
  switch (storage_.ss_family) {
    case AF_INET:
      return ntohs(reinterpret_cast<const sockaddr_in*>(&storage_)->sin_port);
    case AF_INET6:
      return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage_)->sin6_port);
    default:
      return 0;
  }
}

std::optional<InetAddress> SocketAddr::address() const noexcept
{
  switch (storage_.ss_family) {
    case AF_INET: {
      const auto& sin = reinterpret_cast<const sockaddr_in*>(&storage_);
      Inet4Address::addr_type bytes{};
      std::ranges::copy_n(reinterpret_cast<const std::uint8_t*>(&sin->sin_addr), 4, bytes.begin());
      return InetAddress{Inet4Address::from_bytes(bytes)};
    }
    case AF_INET6: {
      const auto& sin6 = reinterpret_cast<const sockaddr_in6*>(&storage_);
      Inet6Address::addr_type bytes{};
      std::ranges::copy_n(reinterpret_cast<const std::uint8_t*>(&sin6->sin6_addr), 16, bytes.begin());
      auto v6 = Inet6Address::from_bytes(bytes);
      if (sin6->sin6_scope_id != 0) {
        v6.set_scope_id(sin6->sin6_scope_id);
      }
      return InetAddress{v6};
    }
    default:
      return std::nullopt;
  }
}

std::string SocketAddr::to_string() const
{
  auto inet = address();
  if (!inet) {
    return "<unspec>";
  }

  auto addr_str = inet->to_string();
  auto p = port();

  if (storage_.ss_family == AF_INET6) {
    return "[" + addr_str + "]:" + std::to_string(p);
  }
  return addr_str + ":" + std::to_string(p);
}
