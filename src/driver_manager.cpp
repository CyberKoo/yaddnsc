//
// Created by Kotarou on 2022/4/5.
//
#include "driver_manager.h"

// POSIX function
#include <dlfcn.h>

#include <string>
#include <algorithm>
#include <filesystem>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "IDriver.h"
#include "driver_ver.h"
#include "logging_pattern.h"
#include "exception/bad_driver_exception.h"

// Internal implementation
class DriverManager::Impl {
public:
    class Driver;

    ~Impl() = default;

    bool is_driver_loaded(std::string_view) const;

    static std::string_view get_driver_name(std::string_view);

    void register_driver(Driver, std::string_view);

public:
    std::map<std::string, Driver> driver_map_;

    std::vector<std::string> loaded_lib_;
};

// Driver RAII class
class DriverManager::Impl::Driver final : public NonCopyable {
public:
    class handle_closer {
    public:
        constexpr void operator()(void *ptr) const {
            if (ptr != nullptr) { dlclose(ptr); }
        }
    };

    using handle_ptr = std::unique_ptr<void, handle_closer>;

    Driver(Driver &&) = default;

    Driver &operator=(Driver &&) = default;

    explicit Driver(std::string_view path) : handle_{dlopen(path.data(), RTLD_LAZY)} {
        if (handle_ == nullptr) {
            SPDLOG_CRITICAL("Unable to load driver {}, error: {}", get_driver_name(path), dlerror());
            throw BadDriverException("loader error");
        }

        // reset error message pointer
        dlerror();

        // load create function
        auto create_func = reinterpret_cast<std::add_pointer_t<IDriver *()>>(dlsym(handle_.get(), "create"));
        if (const auto error = dlerror()) {
            SPDLOG_CRITICAL("Failed to create driver instance, error: {}", error);
            throw BadDriverException("dlsym error");
        }

        driver_ = std::unique_ptr<IDriver>(create_func());
    }

    std::unique_ptr<IDriver> &get() {
        return driver_;
    }

    ~Driver() override {
        driver_.reset();
        handle_.reset();
    }

private:
    handle_ptr handle_;

    std::unique_ptr<IDriver> driver_;
};

DriverManager::~DriverManager() = default;

std::unique_ptr<IDriver> &DriverManager::get_driver(std::string_view name) const {
    if (impl_->driver_map_.contains(name.data())) {
        return impl_->driver_map_.at(name.data()).get();
    }

    SPDLOG_CRITICAL("Driver {} not found", name);
    throw BadDriverException("Driver not found");
}

std::vector<std::string_view> DriverManager::get_loaded_drivers() const {
    std::vector<std::string_view> loaded_drivers;
    std::transform(
        impl_->driver_map_.begin(), impl_->driver_map_.end(), std::back_inserter(loaded_drivers),
        [](const auto &kv) -> std::string_view { return kv.first; }
    );

    return loaded_drivers;
}

void DriverManager::load_driver(std::string_view path) const {
    auto driver_lib_name = impl_->get_driver_name(path);
    SPDLOG_DEBUG("Trying to load driver {}", driver_lib_name);

    if (std::filesystem::exists(path)) {
        if (!impl_->is_driver_loaded(path)) {
            impl_->register_driver(Impl::Driver(path), driver_lib_name);
        } else {
            SPDLOG_WARN("Driver {} already loaded.", driver_lib_name);
        }
    } else {
        SPDLOG_CRITICAL("Failed to load driver {}, file {} not found", driver_lib_name, path);
        throw BadDriverException(driver_lib_name.data());
    }
}

std::string_view DriverManager::Impl::get_driver_name(std::string_view path) {
    auto pos = path.rfind('/');
    return pos != std::string_view::npos && pos + 1 != path.size() ? path.substr(pos + 1, path.size()) : path;
}

bool DriverManager::Impl::is_driver_loaded(std::string_view driver_path) const {
    auto driver_name = get_driver_name(driver_path);
    if (std::ranges::find(loaded_lib_, driver_name) != loaded_lib_.end()) {
        return true;
    }

    return Driver::handle_ptr(dlopen(driver_path.data(), RTLD_NOW | RTLD_NOLOAD)) != nullptr;
}

void DriverManager::Impl::register_driver(Driver driver_res, std::string_view driver_lib_name) {
    auto &driver = driver_res.get();

    // validate driver ABI version
    if (driver->get_driver_version().compare(DRV_VERSION) != 0) {
        SPDLOG_CRITICAL("Driver {} version {} instead of {}.", driver_lib_name, driver->get_driver_version(),
                        DRV_VERSION);

        throw BadDriverException("Driver ABI mismatch");
    }

    // initialize logger
    driver->init_logger(spdlog::get_level(), YADDNSC_LOGGING_PATTERN);
    auto driver_detail = driver->get_detail();
    SPDLOG_INFO("Driver {} loaded, driver name: {}", driver_lib_name, driver_detail.name);
    SPDLOG_DEBUG("Driver {} ({}), developed by {}, version: {}", driver_detail.name, driver_detail.description,
                 driver_detail.author, driver_detail.version);

    driver_map_.emplace(driver_detail.name, std::move(driver_res));
    loaded_lib_.emplace_back(driver_lib_name);
}

DriverManager::DriverManager() : impl_(std::make_unique<Impl>()) {
}
