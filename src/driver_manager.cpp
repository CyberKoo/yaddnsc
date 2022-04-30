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

class DriverManager::Impl {
public:
    class Driver;

    ~Impl() = default;

    bool is_driver_loaded(std::string_view driver_path);

    static std::string_view get_driver_name(std::string_view path);

public:
    std::map<std::string, Driver> _driver_map;

    std::vector<std::string> _loaded_lib;
};

// RAII Driver
class DriverManager::Impl::Driver {
public:
    class handle_closer {
    public:
        void operator()(void *ptr) {
            dlclose(ptr);
        }
    };

    using handle_ptr_t = std::unique_ptr<void, handle_closer>;

    Driver(Driver const &) = delete;

    Driver(Driver &&) = default;

    Driver &operator=(Driver const &) = delete;

    Driver &operator=(Driver &&) = default;

    explicit Driver(std::string_view path) : _handle{dlopen(path.data(), RTLD_LAZY)} {
        if (_handle == nullptr) {
            SPDLOG_CRITICAL("Unable to load driver {}, error: {}", get_driver_name(path), dlerror());
            throw BadDriverException("loader error");
        }

        // reset errors
        dlerror();

        // load create function
        auto create = reinterpret_cast<std::add_pointer<IDriver *()>::type>(dlsym(_handle.get(), "create"));
        if (const auto error = dlerror()) {
            SPDLOG_CRITICAL("Failed to create driver instance, error: {}", dlerror());
            throw BadDriverException("dlsym error");
        }

        _driver = std::unique_ptr<IDriver>(create());
    }

    std::unique_ptr<IDriver> &get() {
        return _driver;
    }

    ~Driver() {
        _driver.reset();
        _handle.reset();
    }

private:
    handle_ptr_t _handle;

    std::unique_ptr<IDriver> _driver;
};

std::unique_ptr<IDriver> &DriverManager::get_driver(std::string_view name) {
    if (_impl->_driver_map.find(name.data()) != _impl->_driver_map.end()) {
        return _impl->_driver_map.at(name.data()).get();
    }

    SPDLOG_CRITICAL("Driver {} not found", name);
    throw BadDriverException("driver not found");
}

std::vector<std::string> DriverManager::get_loaded_drivers() {
    std::vector<std::string> loaded_drivers;
    std::transform(_impl->_driver_map.begin(), _impl->_driver_map.end(), std::back_inserter(loaded_drivers),
                   [](const auto &kv) { return kv.first; });

    return loaded_drivers;
}

void DriverManager::load_driver(std::string_view path) {
    auto driver_lib_name = _impl->get_driver_name(path);
    SPDLOG_DEBUG("Trying to load driver {}", driver_lib_name);

    if (std::filesystem::exists(path)) {
        if (!_impl->is_driver_loaded(path)) {
            auto driver_res = Impl::Driver(path);
            auto &driver = driver_res.get();

            // validate driver ABI version
            if (driver->get_driver_version().compare(DRV_VERSION) != 0) {
                SPDLOG_CRITICAL("Driver {} version {} instead of {}.", driver_lib_name, driver->get_driver_version(),
                                DRV_VERSION);

                throw BadDriverException("driver ABI mismatch");
            }

            // initialize logger
            driver->init_logger(spdlog::get_level(), YADDNSC_LOGGING_PATTERN);
            auto driver_detail = driver->get_detail();
            SPDLOG_INFO("Driver {} loaded, driver name: {}", driver_lib_name, driver_detail.name);
            SPDLOG_DEBUG("Driver {} ({}), developed by {}, version: {}", driver_detail.name, driver_detail.description,
                         driver_detail.author, driver_detail.version);

            _impl->_driver_map.emplace(driver_detail.name, std::move(driver_res));
            _impl->_loaded_lib.emplace_back(driver_lib_name);
        } else {
            SPDLOG_WARN("Driver {} already loaded.", driver_lib_name);
        }
    } else {
        SPDLOG_CRITICAL("Failed to load driver {}, file {} not found", driver_lib_name, path);
        throw BadDriverException(driver_lib_name.data());
    }
}

void DriverManager::reset() {
    _impl->_driver_map.clear();
    _impl->_loaded_lib.clear();
}

std::string_view DriverManager::Impl::get_driver_name(std::string_view path) {
    auto pos = path.rfind('/');
    return (pos != std::string_view::npos && pos + 1 != path.size()) ? path.substr(pos + 1, path.size()) : path;
}

bool DriverManager::Impl::is_driver_loaded(std::string_view driver_path) {
    auto driver_name = get_driver_name(driver_path);
    if (std::find(_loaded_lib.begin(), _loaded_lib.end(), driver_name) != _loaded_lib.end()) {
        return true;
    }

    auto handle = DriverManager::Impl::Driver::handle_ptr_t(dlopen(driver_path.data(), RTLD_NOW | RTLD_NOLOAD));
    return handle != nullptr;
}

DriverManager::DriverManager() : _impl(new Impl) {

}

void DriverManager::ImplDeleter::operator()(DriverManager::Impl *ptr) {
    delete ptr;
}
