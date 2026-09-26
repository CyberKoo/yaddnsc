//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_IP_SOURCE_FACTORY_H
#define YADDNSC_IP_SOURCE_FACTORY_H

#include <expected>
#include <memory>
#include <vector>

#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "infrastructure/ip_source/base.h"

/// IpSourceFactory — constructs the appropriate IpSourceBase implementation from a
///                   subdomain configuration.
///
/// Eliminates the need for callers (e.g. UpdateWorkflow) to branch on Config::IpSource
/// or know about concrete IpSourceBase classes.
namespace IpSourceFactory {
    using Result = std::expected<std::unique_ptr<IpSourceBase>, domain::IpSourceError>;

    /// Create an IP source from subdomain configuration.
    /// @param cfg        The subdomain configuration specifying the IP source type and params.
    /// @param bootstrap  Bootstrap DNS servers handed to HTTP sources for
    ///                   resolving hostname URLs (empty: hostname URLs fail fast).
    /// @return     The appropriate source or a structured creation failure.
    [[nodiscard]] Result create(const domain::SubdomainConfig &cfg,
                                std::vector<Config::DnsServer> bootstrap = {});
} // namespace IpSourceFactory

#endif  // YADDNSC_IP_SOURCE_FACTORY_H
