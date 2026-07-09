//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_IP_SOURCE_FACTORY_H
#define YADDNSC_IP_SOURCE_FACTORY_H

#include <memory>

#include "config/config.h"

#include "base.h"

/// IpSourceFactory — constructs the appropriate IpSourceBase implementation from a
///                   subdomain configuration.
///
/// Eliminates the need for callers (e.g. Updater) to branch on Config::IpSource
/// or know about concrete IpSourceBase classes.
namespace IpSourceFactory {
    /// Create an IP source from subdomain configuration.
    /// @param cfg  The subdomain configuration specifying the IP source type and params.
    /// @return     A unique pointer to the appropriate IpSourceBase implementation.
    [[nodiscard]] std::unique_ptr<IpSourceBase> create(const Config::SubdomainConfig &cfg);
} // namespace IpSourceFactory

#endif  // YADDNSC_IP_SOURCE_FACTORY_H
