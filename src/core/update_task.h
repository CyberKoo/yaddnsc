//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATE_TASK_H
#define YADDNSC_CORE_UPDATE_TASK_H

#include <chrono>
#include <string>

#include "config/config.h"

// ---------------------------------------------------------------------------
// UpdateTask — a self-contained value type describing one DNS record update
//              that the Updater should carry out.
// ---------------------------------------------------------------------------
struct UpdateTask {
    Config::SubdomainConfig config;
    std::string domain_name;
    std::string driver_name;
    std::string fqdn;
    bool force_update{false};
};

#endif // YADDNSC_CORE_UPDATE_TASK_H
