//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_IP_SOURCE_FACTORY_H
#define YADDNSC_IP_SOURCE_FACTORY_H

#include <memory>

#include "base.h"
#include "config/config.h"

// ---------------------------------------------------------------------------
// IpSourceFactory — constructs the appropriate IpSourceBase implementation from a
//                   subdomain configuration.
//
// Eliminates the need for callers (e.g. Updater) to branch on ip_source_type
// or know about concrete IpSourceBase classes.
// ---------------------------------------------------------------------------
namespace IpSourceFactory {
    [[nodiscard]] std::unique_ptr<IpSourceBase> create(const Config::subdomain_config &cfg);
}

#endif // YADDNSC_IP_SOURCE_FACTORY_H
