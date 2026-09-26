//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_IP_SOURCE_FACTORY_H
#define YADDNSC_IP_SOURCE_FACTORY_H

#include <memory>

#include <expected>

#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "infrastructure/ip_source/base.h"
#include "infrastructure/network/http/types.h"

/// IpSourceFactory — constructs the appropriate IpSourceBase implementation from a
///                   subdomain configuration.
///
/// Eliminates the need for callers (e.g. UpdateWorkflow) to branch on Config::IpSource
/// or know about concrete IpSourceBase classes.
namespace IpSourceFactory {
using Result = std::expected<std::unique_ptr<IpSourceBase>, domain::IpSourceError>;

/// Create an IP source from subdomain configuration.
/// @param cfg          The subdomain configuration specifying the IP source type and params.
/// @param http_options Shared HTTP policy built once by the composition root
///                     (used only by HTTP sources; an empty/default policy
///                     makes hostname URLs fail fast at resolve time).
/// @return     The appropriate source or a structured creation failure.
[[nodiscard]] Result create(const domain::SubdomainConfig& cfg, net::http::Options http_options = {});
}  // namespace IpSourceFactory

#endif  // YADDNSC_IP_SOURCE_FACTORY_H
