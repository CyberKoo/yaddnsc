//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATE_TASK_H
#define YADDNSC_CORE_UPDATE_TASK_H

#include <string>

#include "config/config.h"

/// UpdateTask — a self-contained value type describing one DNS record update
///              that the Updater should carry out.
struct UpdateTask {
    Config::SubdomainConfig config; ///< Per-subdomain configuration
    std::string domain_name;        ///< Parent domain name
    std::string driver_name;        ///< Name of the driver plugin to use
    std::string fqdn;               ///< Fully qualified domain name
    bool force_update{false};       ///< Skip IP-change check; always send update
};

#endif // YADDNSC_CORE_UPDATE_TASK_H
