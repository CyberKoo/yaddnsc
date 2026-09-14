//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_IP_SOURCE_FACTORY_H
#define YADDNSC_IP_SOURCE_FACTORY_H

#include <memory>

namespace Config { struct SubdomainConfig; }

class IpSourceBase;

namespace Utils { class CancellationToken; }

/// IpSourceFactory — constructs the appropriate IpSourceBase implementation from a
///                   subdomain configuration.
///
/// Eliminates the need for callers (e.g. Updater) to branch on Config::IpSource
/// or know about concrete IpSourceBase classes.
namespace IpSourceFactory {
    /// Create an IP source from subdomain configuration.
    /// @param cfg    The subdomain configuration specifying the IP source type and params.
    /// @param token  Cancellation token (HTTP source only; inert for others).
    /// @return       A unique pointer to the appropriate IpSourceBase implementation.
    [[nodiscard]] std::unique_ptr<IpSourceBase> create(const Config::SubdomainConfig &cfg,
                                                       const Utils::CancellationToken &token);
} // namespace IpSourceFactory

#endif  // YADDNSC_IP_SOURCE_FACTORY_H
