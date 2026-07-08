//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_HTTP_IP_SOURCE_H
#define YADDNSC_HTTP_IP_SOURCE_H

#include <memory>
#include <string>

#include "address_family.h"
#include "base.h"

class PersistentHttpClient;

/// HttpIpSource — fetches the local public IP address from an external HTTP service.
///
/// Uses PersistentHttpClient to maintain a keep-alive connection across
/// resolve() calls, which is more efficient than opening a new connection
/// each time.  The address family and outbound interface binding are passed
/// through to the underlying HTTP client.
///
/// resolve() returns 0 or 1 addresses.
class HttpIpSource final : public IpSourceBase
{
public:
  /// Construct with an HTTP URL and optional filtering parameters.
  /// @param url              URL of the HTTP IP detection service.
  /// @param address_family   Preferred address family for the connection.
  /// @param bind_interface   Outbound network interface to bind to (empty = any).
  explicit HttpIpSource(std::string url,
                        AddressFamily address_family = AddressFamily::UNSPECIFIED,
                        std::string bind_interface = {});

  ~HttpIpSource() override;

  [[nodiscard]] std::vector<InetAddress> resolve() const override;

private:
  std::string url_;
  AddressFamily address_family_;
  std::string bind_interface_;
  std::unique_ptr<PersistentHttpClient> client_;
};

#endif  // YADDNSC_HTTP_IP_SOURCE_H
