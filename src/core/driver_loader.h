//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_CORE_DRIVER_LOADER_H
#define YADDNSC_CORE_DRIVER_LOADER_H

namespace Config {
    struct AppConfig;
}

class DriverManager;

// ---------------------------------------------------------------------------
// DriverLoader — loads all configured DDNS driver shared libraries.
//
// Extracted from Manager::Impl to keep driver-loading logic independent of
// the scheduler and signal-handling concerns.
// ---------------------------------------------------------------------------
struct DriverLoader {
    static void load(DriverManager &driver_manager, const Config::AppConfig &config);
};

#endif // YADDNSC_CORE_DRIVER_LOADER_H
