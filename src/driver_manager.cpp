//
// Created by Kotarou on 2022/4/5.
//
#include "driver_manager.h"

#include <string>
#include <filesystem>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <dlfcn.h>

#include "IDriver.h"
#include "driver_ver.h"
#include "logging_pattern.h"
#include "exception/bad_driver_exception.h"

DriverManager::~DriverManager() {
    // destroy all drivers before dlclose
    for (auto &[name, driver]: _driver_map) {
        driver.reset();
    }

    // close all handles
    _handlers.clear();
}

void DriverManager::load_driver(std::string_view path) {
    auto driver_lib_name = get_driver_name(path);
    SPDLOG_DEBUG("Trying to load driver {}", driver_lib_name);

    if (std::filesystem::exists(path)) {
        if (!is_driver_loaded(path)) {
            auto handle = load_external_dynamic_library(path);
            auto driver = std::unique_ptr<IDriver>(get_instance(handle));

            // validate driver ABI version
            if (driver->get_driver_version().compare(DRV_VERSION) != 0) {
                SPDLOG_CRITICAL("Driver {} version {} instead of {}.", driver_lib_name, driver->get_driver_version(),
                                DRV_VERSION);

                throw BadDriverException("driver ABI mismatch");
            }

            // initialize logger
            driver->init_logger(spdlog::get_level(), _SPDLOG_LOGGING_PATTERN);
            auto driver_detail = driver->get_detail();
            SPDLOG_INFO("Driver {} loaded, driver name: {}", driver_lib_name, driver_detail.name);
            SPDLOG_DEBUG("Driver {} ({}), developed by {}, version: {}", driver_detail.name, driver_detail.description,
                         driver_detail.author, driver_detail.version);

            _handlers.emplace_back(std::move(handle));
            _driver_map.emplace(std::make_pair(driver_detail.name, std::move(driver)));
        } else {
            SPDLOG_WARN("Driver {} already loaded.", driver_lib_name);
        }
    } else {
        SPDLOG_ERROR("Failed to load driver {}, file {} not found", driver_lib_name, path);
        throw BadDriverException(driver_lib_name.data());
    }
}

DriverManager::handle_ptr_t DriverManager::load_external_dynamic_library(std::string_view path) {
    auto handle = handle_ptr_t(dlopen(path.data(), RTLD_LAZY));

    if (handle == nullptr) {
        SPDLOG_CRITICAL("Unable to load driver {}, error: {}", get_driver_name(path), dlerror());
        throw BadDriverException("loader error");
    }

    return handle;
}

IDriver *DriverManager::get_instance(handle_ptr_t &handle) {
    // reset errors
    dlerror();

    // load create function
    auto create = reinterpret_cast<std::add_pointer<IDriver *()>::type>(dlsym(handle.get(), "create"));
    if (const auto error = dlerror()) {
        SPDLOG_CRITICAL("Failed to create driver instance, error: {}", dlerror());
        throw BadDriverException("dlsym error");
    }

    // return class instance
    return create();
}

std::string_view DriverManager::get_driver_name(std::string_view path) {
    auto pos = path.rfind('/');

    if (pos != std::string_view::npos && pos + 1 != path.size()) {
        return path.substr(pos + 1, path.size());
    } else {
        return path;
    }
}

bool DriverManager::is_driver_loaded(std::string_view driver_path) {
    handle_ptr_t handle = handle_ptr_t(dlopen(driver_path.data(), RTLD_NOW | RTLD_NOLOAD));

    return handle != nullptr;
}

std::unique_ptr<IDriver> &DriverManager::get_driver(std::string_view name) {
    if (_driver_map.find(name.data()) != _driver_map.end()) {
        return _driver_map[name.data()];
    }

    SPDLOG_CRITICAL("Driver {} not found", name);
    throw BadDriverException("driver not found");
}

std::vector<std::string> DriverManager::get_loaded_drivers() {
    std::vector<std::string> loaded_drivers;
    std::transform(_driver_map.begin(), _driver_map.end(), std::back_inserter(loaded_drivers),
                   [](const auto &kv) { return kv.first; });

    return loaded_drivers;
}

void DriverManager::handle_closer::operator()(void *handle) {
    dlclose(handle);
}
