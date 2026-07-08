//
// Created by Kotarou on 2026/7/1.
//

#include "iface_util.h"

#include <algorithm>
#include <map>
#include <stdexcept>

#include "network/inet_address.h"
#include "network/net_devices.h"
#include "util/cache.hpp"

#include "fmt.hpp"

// ===========================================================================
// Internal cache
// ===========================================================================

namespace
{
using InterfaceMap = std::map<std::string, std::vector<InetAddress>>;

[[nodiscard]] InterfaceMap get_cached_interfaces()
{
  static Utils::Cache::TtlCache<std::monostate, InterfaceMap> cache(std::chrono::seconds(5));
  return cache.get_or_compute(std::monostate{}, [] { return NetDevices::enumerate_interfaces(); });
}
}  // anonymous namespace

// ===========================================================================
// Public API
// ===========================================================================

std::vector<std::string> InterfaceUtil::get_interfaces()
{
  auto interface_map = get_cached_interfaces();
  std::vector<std::string> interfaces;
  interfaces.reserve(interface_map.size());
  std::ranges::transform(interface_map, std::back_inserter(interfaces), [](const auto& kv) { return kv.first; });
  return interfaces;
}

std::vector<InetAddress> InterfaceUtil::get_addresses(const std::string& interface_name)
{
  auto all = get_cached_interfaces();
  if (const auto it = all.find(interface_name); it != all.end()) {
    return it->second;
  }
  throw std::runtime_error(fmt::format("Interface {} not found", interface_name));
}
